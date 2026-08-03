#pragma once

#include <algorithm>
#include <memory>
#include <vector>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "executor_utils.h"
#include "index/ix.h"
#include "system/sm.h"

class SortExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> cols_;
    std::vector<ColMeta> sort_cols_;
    std::vector<bool> is_desc_;
    int limit_;
    std::vector<std::unique_ptr<RmRecord>> tuples_;
    size_t cursor_ = 0;

   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<TabCol> sort_cols,
                 std::vector<bool> is_desc, int limit) {
        prev_ = std::move(prev);
        cols_ = prev_->cols();
        for (auto &sort_col : sort_cols) {
            sort_cols_.push_back(prev_->get_col_offset(sort_col));
        }
        is_desc_ = std::move(is_desc);
        limit_ = limit;
    }

    void beginTuple() override {
        tuples_.clear();
        cursor_ = 0;
        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            tuples_.push_back(prev_->Next());
        }
        std::stable_sort(tuples_.begin(), tuples_.end(), [&](const auto &lhs, const auto &rhs) {
            for (size_t i = 0; i < sort_cols_.size(); ++i) {
                const auto &col = sort_cols_[i];
                int cmp = compare_raw_value(lhs->data + col.offset, rhs->data + col.offset, col.type, col.len);
                if (cmp != 0) {
                    return is_desc_[i] ? cmp > 0 : cmp < 0;
                }
            }
            return false;
        });
        if (limit_ >= 0 && tuples_.size() > static_cast<size_t>(limit_)) {
            tuples_.resize(limit_);
        }
    }

    void nextTuple() override {
        if (!is_end()) {
            ++cursor_;
        }
    }

    bool is_end() const override { return cursor_ >= tuples_.size(); }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*tuples_[cursor_]);
    }

    Rid &rid() override { return _abstract_rid; }

    size_t tupleLen() const override { return prev_->tupleLen(); }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
};
