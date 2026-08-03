/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <algorithm>
#include <cstring>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "executor_utils.h"
#include "index/ix.h"
#include "system/sm.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    size_t len_;
    std::vector<ColMeta> cols_;
    std::vector<Condition> fed_conds_;
    bool isend;
    std::vector<std::unique_ptr<RmRecord>> left_block_;
    std::vector<std::unique_ptr<RmRecord>> right_cache_;
    std::unique_ptr<RmRecord> cache_left_rec_;
    std::unique_ptr<RmRecord> right_rec_;
    std::unique_ptr<RmRecord> current_;
    size_t left_idx_;
    int right_idx_;
    bool use_right_cache_;
    static constexpr size_t JOIN_BUFFER_BYTES = 8 * 1024 * 1024;

    std::unique_ptr<RmRecord> make_join_record(const RmRecord *left_rec, const RmRecord *right_rec) {
        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, left_rec->data, left_->tupleLen());
        memcpy(rec->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
        return rec;
    }

    bool load_left_block() {
        left_block_.clear();
        left_idx_ = 0;
        size_t bytes = 0;
        size_t row_len = std::max<size_t>(left_->tupleLen(), 1);
        size_t max_rows = std::max<size_t>(1, JOIN_BUFFER_BYTES / row_len);
        while (!left_->is_end() && left_block_.size() < max_rows &&
               (left_block_.empty() || bytes + row_len <= JOIN_BUFFER_BYTES)) {
            left_block_.push_back(left_->Next());
            bytes += row_len;
            left_->nextTuple();
        }
        return !left_block_.empty();
    }

    bool try_load_right_cache() {
        right_cache_.clear();
        size_t bytes = 0;
        size_t row_len = std::max<size_t>(right_->tupleLen(), 1);
        for (; !right_->is_end(); right_->nextTuple()) {
            if (!right_cache_.empty() && bytes + row_len > JOIN_BUFFER_BYTES) {
                right_cache_.clear();
                return false;
            }
            right_cache_.push_back(right_->Next());
            bytes += row_len;
        }
        return !right_cache_.empty();
    }

    void set_cache_current() {
        if (cache_left_rec_ == nullptr || right_idx_ < 0) {
            isend = true;
            current_.reset();
            return;
        }
        current_ = make_join_record(cache_left_rec_.get(), right_cache_[right_idx_].get());
        isend = false;
    }

    void advance_to_match() {
        current_.reset();
        while (!left_block_.empty()) {
            while (!right_->is_end()) {
                if (right_rec_ == nullptr) {
                    right_rec_ = right_->Next();
                }
                while (left_idx_ < left_block_.size()) {
                    auto rec = make_join_record(left_block_[left_idx_].get(), right_rec_.get());
                    ++left_idx_;
                    if (eval_conditions(cols_, rec.get(), fed_conds_)) {
                        current_ = std::move(rec);
                        isend = false;
                        return;
                    }
                }
                right_->nextTuple();
                right_rec_.reset();
                left_idx_ = 0;
            }
            if (!load_left_block()) {
                break;
            }
            right_->beginTuple();
            right_rec_.reset();
        }
        isend = true;
    }

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                           std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }
        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        fed_conds_ = std::move(conds);
        left_idx_ = 0;
        right_idx_ = -1;
        use_right_cache_ = false;
    }

    void beginTuple() override {
        left_->beginTuple();
        right_->beginTuple();
        right_rec_.reset();
        current_.reset();
        cache_left_rec_.reset();
        use_right_cache_ = fed_conds_.empty() && try_load_right_cache();
        if (use_right_cache_) {
            if (left_->is_end()) {
                isend = true;
                return;
            }
            cache_left_rec_ = left_->Next();
            right_idx_ = static_cast<int>(right_cache_.size()) - 1;
            set_cache_current();
            return;
        }
        if (fed_conds_.empty()) {
            right_->beginTuple();
        }
        if (!load_left_block()) {
            isend = true;
            return;
        }
        advance_to_match();
    }

    void nextTuple() override {
        if (isend) {
            return;
        }
        if (use_right_cache_) {
            if (right_idx_ > 0) {
                --right_idx_;
            } else {
                left_->nextTuple();
                if (left_->is_end()) {
                    isend = true;
                    current_.reset();
                    return;
                }
                cache_left_rec_ = left_->Next();
                right_idx_ = static_cast<int>(right_cache_.size()) - 1;
            }
            set_cache_current();
            return;
        }
        advance_to_match();
    }

    bool is_end() const override { return isend; }

    std::unique_ptr<RmRecord> Next() override {
        if (current_ == nullptr) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*current_);
    }

    Rid &rid() override { return _abstract_rid; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
};
