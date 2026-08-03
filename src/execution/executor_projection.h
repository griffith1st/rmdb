/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <cstring>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class ProjectionExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> cols_;
    size_t len_;
    std::vector<size_t> sel_idxs_;

   public:
    ProjectionExecutor(std::unique_ptr<AbstractExecutor> prev, const std::vector<TabCol> &sel_cols) {
        prev_ = std::move(prev);
        size_t curr_offset = 0;
        auto &prev_cols = prev_->cols();
        for (auto &sel_col : sel_cols) {
            ColMeta col;
            if (sel_col.agg_type == AGG_COUNT && sel_col.agg_star) {
                col = {.tab_name = "", .name = sel_col.alias, .type = TYPE_INT, .len = static_cast<int>(sizeof(int)),
                       .offset = static_cast<int>(curr_offset), .index = false};
                sel_idxs_.push_back(static_cast<size_t>(-1));
            } else {
                auto pos = get_col(prev_cols, sel_col);
                sel_idxs_.push_back(pos - prev_cols.begin());
                col = *pos;
                col.offset = curr_offset;
                if (sel_col.agg_type != AGG_NONE && !sel_col.alias.empty()) {
                    col.name = sel_col.alias;
                }
            }
            curr_offset += col.len;
            cols_.push_back(col);
        }
        len_ = curr_offset;
    }

    void beginTuple() override { prev_->beginTuple(); }

    void nextTuple() override { prev_->nextTuple(); }

    bool is_end() const override { return prev_->is_end(); }

    std::unique_ptr<RmRecord> Next() override {
        auto prev_rec = prev_->Next();
        auto rec = std::make_unique<RmRecord>(len_);
        auto &prev_cols = prev_->cols();
        for (size_t i = 0; i < sel_idxs_.size(); ++i) {
            if (sel_idxs_[i] == static_cast<size_t>(-1)) {
                int placeholder = 0;
                memcpy(rec->data + cols_[i].offset, &placeholder, sizeof(int));
                continue;
            }
            const ColMeta &src_col = prev_cols[sel_idxs_[i]];
            memcpy(rec->data + cols_[i].offset, prev_rec->data + src_col.offset, src_col.len);
        }
        return rec;
    }

    Rid &rid() override { return prev_->rid(); }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
};
