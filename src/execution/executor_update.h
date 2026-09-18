/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <cstring>
#include <cmath>
#include <limits>
#include <set>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "index_utils.h"
#include "system/sm.h"

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;

    void apply_assignment(const SetClause &assignment, const RmRecord &old_record, RmRecord &record) {
        auto target = tab_.get_col(assignment.lhs.col_name);
        if (assignment.arithmetic == 0) {
            memcpy(record.data + target->offset, assignment.rhs.raw->data, target->len);
            return;
        }
        auto source = tab_.get_col(assignment.source.col_name);
        const char *input = old_record.data + source->offset;
        char *output = record.data + target->offset;
        if (target->type == TYPE_FLOAT) {
            float lhs;
            memcpy(&lhs, input, sizeof(lhs));
            float result = assignment.arithmetic == '+' ? lhs + assignment.rhs.float_val : lhs - assignment.rhs.float_val;
            if (!std::isfinite(result)) throw InternalError("UPDATE arithmetic overflow");
            memcpy(output, &result, sizeof(result));
        } else {
            int64_t lhs = 0;
            int64_t rhs = target->type == TYPE_INT ? assignment.rhs.int_val : assignment.rhs.bigint_val;
            if (target->type == TYPE_INT) {
                int value;
                memcpy(&value, input, sizeof(value));
                lhs = value;
            } else {
                memcpy(&lhs, input, sizeof(lhs));
            }
            int64_t result;
            bool overflow = assignment.arithmetic == '+' ? __builtin_add_overflow(lhs, rhs, &result)
                                                         : __builtin_sub_overflow(lhs, rhs, &result);
            if (overflow || (target->type == TYPE_INT &&
                (result < std::numeric_limits<int>::min() || result > std::numeric_limits<int>::max()))) {
                throw InternalError("UPDATE arithmetic overflow");
            }
            if (target->type == TYPE_INT) {
                int value = static_cast<int>(result);
                memcpy(output, &value, sizeof(value));
            } else {
                memcpy(output, &result, sizeof(result));
            }
        }
    }

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
    }

    std::unique_ptr<RmRecord> Next() override {
        if (context_ != nullptr && context_->lock_mgr_ != nullptr && context_->txn_ != nullptr) {
            context_->lock_mgr_->lock_exclusive_on_table(context_->txn_, fh_->GetFd());
        }
        // Validate the entire statement before changing any table or index.
        // All SET expressions read the original tuple, including multiple SETs.
        std::vector<std::unique_ptr<RmRecord>> old_records, new_records;
        for (const auto &rid : rids_) {
            old_records.push_back(fh_->get_record(rid, context_));
            new_records.push_back(std::make_unique<RmRecord>(*old_records.back()));
            for (const auto &assignment : set_clauses_) {
                apply_assignment(assignment, *old_records.back(), *new_records.back());
            }
        }
        auto txn = context_ ? context_->txn_ : nullptr;
        for (const auto &index : tab_.indexes) {
            auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
            auto less = [&index](const std::string &lhs, const std::string &rhs) {
                int offset = 0;
                for (const auto &col : index.cols) {
                    int cmp = ix_compare(lhs.data() + offset, rhs.data() + offset, col.type, col.len);
                    if (cmp) return cmp < 0;
                    offset += col.len;
                }
                return false;
            };
            std::set<std::string, decltype(less)> proposed(less);
            for (size_t i = 0; i < rids_.size(); ++i) {
                auto key = make_index_key(index, new_records[i]->data);
                if (!proposed.emplace(key.get(), index.col_tot_len).second) {
                    throw InternalError("Duplicate index key");
                }
                std::vector<Rid> existing;
                if (ih->get_value(key.get(), &existing, txn)) {
                    for (const auto &rid : existing) {
                        if (std::find(rids_.begin(), rids_.end(), rid) == rids_.end()) {
                            throw InternalError("Duplicate index key");
                        }
                    }
                }
            }
        }
        // Log all row images before a dirty data page can be evicted.
        for (size_t i = 0; i < rids_.size(); ++i) {
            if (context_ != nullptr && context_->txn_ != nullptr && context_->log_mgr_ != nullptr) {
                UpdateLogRecord log_record(txn->get_transaction_id(), *old_records[i], *new_records[i], rids_[i], tab_name_);
                log_record.prev_lsn_ = context_->txn_->get_prev_lsn();
                lsn_t lsn = context_->log_mgr_->add_log_to_buffer(&log_record);
                context_->txn_->set_prev_lsn(lsn);
            }
        }
        if (context_ && context_->log_mgr_) context_->log_mgr_->flush_log_to_disk();
        for (const auto &index : tab_.indexes) {
            auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
            for (const auto &rec : old_records) {
                auto key = make_index_key(index, rec->data);
                ih->delete_entry(key.get(), txn);
            }
        }
        for (size_t i = 0; i < rids_.size(); ++i) {
            if (txn) txn->append_write_record(new WriteRecord(WType::UPDATE_TUPLE, tab_name_, rids_[i], *old_records[i]));
            fh_->update_record(rids_[i], new_records[i]->data, context_);
        }
        for (const auto &index : tab_.indexes) {
            auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
            for (size_t i = 0; i < rids_.size(); ++i) {
                auto key = make_index_key(index, new_records[i]->data);
                ih->insert_entry(key.get(), rids_[i], txn);
            }
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
