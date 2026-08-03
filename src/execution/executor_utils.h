#pragma once

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include "executor_abstract.h"

inline int compare_raw_value(const char *lhs, const char *rhs, ColType type, int len) {
    if (type == TYPE_INT) {
        int l = *reinterpret_cast<const int *>(lhs);
        int r = *reinterpret_cast<const int *>(rhs);
        return (l > r) - (l < r);
    }
    if (type == TYPE_BIGINT) {
        int64_t l = *reinterpret_cast<const int64_t *>(lhs);
        int64_t r = *reinterpret_cast<const int64_t *>(rhs);
        return (l > r) - (l < r);
    }
    if (type == TYPE_DATETIME) {
        int64_t l = *reinterpret_cast<const int64_t *>(lhs);
        int64_t r = *reinterpret_cast<const int64_t *>(rhs);
        return (l > r) - (l < r);
    }
    if (type == TYPE_FLOAT) {
        float l = *reinterpret_cast<const float *>(lhs);
        float r = *reinterpret_cast<const float *>(rhs);
        return (l > r) - (l < r);
    }
    return strncmp(lhs, rhs, len);
}

inline bool compare_result(int cmp, CompOp op) {
    switch (op) {
        case OP_EQ:
            return cmp == 0;
        case OP_NE:
            return cmp != 0;
        case OP_LT:
            return cmp < 0;
        case OP_GT:
            return cmp > 0;
        case OP_LE:
            return cmp <= 0;
        case OP_GE:
            return cmp >= 0;
    }
    return false;
}

inline bool eval_conditions(const std::vector<ColMeta> &cols, const RmRecord *rec,
                            const std::vector<Condition> &conds) {
    for (auto &cond : conds) {
        auto lhs = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &col) {
            return col.tab_name == cond.lhs_col.tab_name && col.name == cond.lhs_col.col_name;
        });
        if (lhs == cols.end()) {
            throw ColumnNotFoundError(cond.lhs_col.tab_name + "." + cond.lhs_col.col_name);
        }
        const char *lhs_data = rec->data + lhs->offset;
        const char *rhs_data = nullptr;
        if (cond.is_rhs_val) {
            rhs_data = cond.rhs_val.raw->data;
        } else {
            auto rhs = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &col) {
                return col.tab_name == cond.rhs_col.tab_name && col.name == cond.rhs_col.col_name;
            });
            if (rhs == cols.end()) {
                throw ColumnNotFoundError(cond.rhs_col.tab_name + "." + cond.rhs_col.col_name);
            }
            rhs_data = rec->data + rhs->offset;
        }
        if (!compare_result(compare_raw_value(lhs_data, rhs_data, lhs->type, lhs->len), cond.op)) {
            return false;
        }
    }
    return true;
}
