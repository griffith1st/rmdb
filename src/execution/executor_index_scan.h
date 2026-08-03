/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <limits>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "executor_utils.h"
#include "index/ix.h"
#include "system/sm.h"

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;                      // 表名称
    TabMeta tab_;                               // 表的元数据
    std::vector<Condition> conds_;              // 扫描条件
    RmFileHandle *fh_;                          // 表的数据文件句柄
    std::vector<ColMeta> cols_;                 // 需要读取的字段
    size_t len_;                                // 选取出来的一条记录的长度
    std::vector<Condition> fed_conds_;          // 扫描条件，和conds_字段相同

    std::vector<std::string> index_col_names_;  // index scan涉及到的索引包含的字段
    IndexMeta index_meta_;                      // index scan涉及到的索引元数据

    Rid rid_;
    std::unique_ptr<RecScan> scan_;
    std::vector<Rid> rids_;
    size_t cursor_ = 0;

    SmManager *sm_manager_;

    static void fill_min(char *buf, const ColMeta &col) {
        if (col.type == TYPE_INT) {
            int v = std::numeric_limits<int>::min();
            memcpy(buf, &v, sizeof(v));
        } else if (col.type == TYPE_BIGINT || col.type == TYPE_DATETIME) {
            int64_t v = std::numeric_limits<int64_t>::min();
            memcpy(buf, &v, sizeof(v));
        } else if (col.type == TYPE_FLOAT) {
            float v = -std::numeric_limits<float>::max();
            memcpy(buf, &v, sizeof(v));
        } else {
            memset(buf, 0, col.len);
        }
    }

    static void fill_max(char *buf, const ColMeta &col) {
        if (col.type == TYPE_INT) {
            int v = std::numeric_limits<int>::max();
            memcpy(buf, &v, sizeof(v));
        } else if (col.type == TYPE_BIGINT || col.type == TYPE_DATETIME) {
            int64_t v = std::numeric_limits<int64_t>::max();
            memcpy(buf, &v, sizeof(v));
        } else if (col.type == TYPE_FLOAT) {
            float v = std::numeric_limits<float>::max();
            memcpy(buf, &v, sizeof(v));
        } else {
            memset(buf, 0xff, col.len);
        }
    }

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, std::vector<std::string> index_col_names,
                    Context *context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        // index_no_ = index_no;
        index_col_names_ = index_col_names; 
        index_meta_ = *(tab_.get_index_meta(index_col_names_));
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;
        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };

        for (auto &cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                // lhs is on other table, now rhs must be on this table
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                // swap lhs and rhs
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
    }

    void beginTuple() override {
        if (context_ != nullptr && context_->lock_mgr_ != nullptr && context_->txn_ != nullptr) {
            context_->lock_mgr_->lock_shared_on_table(context_->txn_, fh_->GetFd());
        }
        rids_.clear();
        cursor_ = 0;

        std::vector<char> lower(index_meta_.col_tot_len), upper(index_meta_.col_tot_len);
        int offset = 0;
        for (auto &col : index_meta_.cols) {
            fill_min(lower.data() + offset, col);
            fill_max(upper.data() + offset, col);
            offset += col.len;
        }

        bool has_lower = false;
        bool has_upper = false;
        bool lower_inclusive = true;
        bool upper_inclusive = true;
        offset = 0;
        for (auto &idx_col : index_meta_.cols) {
            bool has_condition = false;
            bool has_range = false;
            for (auto &cond : fed_conds_) {
                if (!cond.is_rhs_val || cond.lhs_col.tab_name != tab_name_ || cond.lhs_col.col_name != idx_col.name) {
                    continue;
                }
                has_condition = true;
                const char *rhs = cond.rhs_val.raw->data;
                if (cond.op == OP_EQ) {
                    memcpy(lower.data() + offset, rhs, idx_col.len);
                    memcpy(upper.data() + offset, rhs, idx_col.len);
                    has_lower = has_upper = true;
                    lower_inclusive = upper_inclusive = true;
                } else if (cond.op == OP_GT || cond.op == OP_GE) {
                    if (!has_lower || compare_raw_value(rhs, lower.data() + offset, idx_col.type, idx_col.len) > 0) {
                        memcpy(lower.data() + offset, rhs, idx_col.len);
                        lower_inclusive = cond.op == OP_GE;
                    }
                    has_lower = true;
                    has_range = true;
                } else if (cond.op == OP_LT || cond.op == OP_LE) {
                    if (!has_upper || compare_raw_value(rhs, upper.data() + offset, idx_col.type, idx_col.len) < 0) {
                        memcpy(upper.data() + offset, rhs, idx_col.len);
                        upper_inclusive = cond.op == OP_LE;
                    }
                    has_upper = true;
                    has_range = true;
                }
            }
            if (!has_condition || has_range) {
                break;
            }
            offset += idx_col.len;
        }

        auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_meta_.cols)).get();
        ih->get_range(lower.data(), lower_inclusive, has_lower, upper.data(), upper_inclusive, has_upper, &rids_);
        while (!is_end()) {
            auto rec = fh_->get_record(rids_[cursor_], context_);
            if (eval_conditions(cols_, rec.get(), fed_conds_)) {
                rid_ = rids_[cursor_];
                break;
            }
            ++cursor_;
        }
    }

    void nextTuple() override {
        if (is_end()) {
            return;
        }
        ++cursor_;
        while (!is_end()) {
            auto rec = fh_->get_record(rids_[cursor_], context_);
            if (eval_conditions(cols_, rec.get(), fed_conds_)) {
                rid_ = rids_[cursor_];
                break;
            }
            ++cursor_;
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        return fh_->get_record(rids_[cursor_], context_);
    }

    bool is_end() const override { return cursor_ >= rids_.size(); }

    Rid &rid() override { return rid_; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
};
