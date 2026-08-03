/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "analyze.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <map>

#include "common/datetime_utils.h"

static void coerce_value_to_col(Value &val, const ColMeta &col) {
    if (col.type == val.type) {
        val.init_raw(col.len);
        return;
    }
    if (col.type == TYPE_BIGINT && val.type == TYPE_INT) {
        int int_val = val.int_val;
        val.set_bigint(static_cast<int64_t>(int_val));
        val.init_raw(col.len);
        return;
    }
    if (col.type == TYPE_DATETIME && val.type == TYPE_STRING) {
        val.set_datetime(parse_datetime_to_int64(val.str_val));
        val.init_raw(col.len);
        return;
    }
    if (col.type == TYPE_FLOAT && val.type == TYPE_INT) {
        int int_val = val.int_val;
        val.set_float(static_cast<float>(int_val));
        val.init_raw(col.len);
        return;
    }
    throw IncompatibleTypeError(coltype2str(col.type), coltype2str(val.type));
}

std::shared_ptr<Query> Analyze::do_analyze(std::shared_ptr<ast::TreeNode> parse) {
    std::shared_ptr<Query> query = std::make_shared<Query>();
    if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(parse)) {
        query->tables = std::move(x->tabs);
        for (auto &tab_name : query->tables) {
            if (!sm_manager_->db_.is_table(tab_name)) {
                throw TableNotFoundError(tab_name);
            }
        }

        std::vector<ColMeta> all_cols;
        get_all_cols(query->tables, all_cols);
        for (auto &sv_sel_col : x->cols) {
            TabCol tab_col{.tab_name = sv_sel_col->tab_name,
                           .col_name = sv_sel_col->col_name,
                           .agg_type = sv_sel_col->agg_type,
                           .agg_star = sv_sel_col->agg_star,
                           .alias = sv_sel_col->alias};
            if (tab_col.agg_type != AGG_NONE) {
                if (!tab_col.agg_star) {
                    tab_col = check_column(all_cols, tab_col);
                    if (tab_col.agg_type == AGG_SUM) {
                        TabMeta &tab = sm_manager_->db_.get_table(tab_col.tab_name);
                        auto col = tab.get_col(tab_col.col_name);
                        if (col->type != TYPE_INT && col->type != TYPE_FLOAT) {
                            throw IncompatibleTypeError("SUM", coltype2str(col->type));
                        }
                    }
                }
                query->cols.push_back(tab_col);
            } else {
                query->cols.push_back(tab_col);
            }
        }
        if (query->cols.empty()) {
            for (auto &col : all_cols) {
                query->cols.push_back({.tab_name = col.tab_name, .col_name = col.name});
            }
        } else {
            for (auto &sel_col : query->cols) {
                if (sel_col.agg_type == AGG_NONE) {
                    sel_col = check_column(all_cols, sel_col);
                }
            }
        }

        get_clause(x->conds, query->conds);
        check_clause(query->tables, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(parse)) {
        if (!sm_manager_->db_.is_table(x->tab_name)) {
            throw TableNotFoundError(x->tab_name);
        }
        query->tables = {x->tab_name};
        TabMeta &tab = sm_manager_->db_.get_table(x->tab_name);
        for (auto &sv_set_clause : x->set_clauses) {
            auto col = tab.get_col(sv_set_clause->col_name);
            SetClause set_clause;
            set_clause.lhs = {.tab_name = x->tab_name, .col_name = sv_set_clause->col_name};
            set_clause.rhs = convert_sv_value(sv_set_clause->val);
            coerce_value_to_col(set_clause.rhs, *col);
            query->set_clauses.push_back(set_clause);
        }
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(parse)) {
        if (!sm_manager_->db_.is_table(x->tab_name)) {
            throw TableNotFoundError(x->tab_name);
        }
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(parse)) {
        if (!sm_manager_->db_.is_table(x->tab_name)) {
            throw TableNotFoundError(x->tab_name);
        }
        for (auto &sv_val : x->vals) {
            query->values.push_back(convert_sv_value(sv_val));
        }
    }
    query->parse = std::move(parse);
    return query;
}

TabCol Analyze::check_column(const std::vector<ColMeta> &all_cols, TabCol target) {
    if (target.tab_name.empty()) {
        std::string tab_name;
        for (auto &col : all_cols) {
            if (col.name == target.col_name) {
                if (!tab_name.empty()) {
                    throw AmbiguousColumnError(target.col_name);
                }
                tab_name = col.tab_name;
            }
        }
        if (tab_name.empty()) {
            throw ColumnNotFoundError(target.col_name);
        }
        target.tab_name = tab_name;
    } else {
        auto pos = std::find_if(all_cols.begin(), all_cols.end(), [&](const ColMeta &col) {
            return col.tab_name == target.tab_name && col.name == target.col_name;
        });
        if (pos == all_cols.end()) {
            throw ColumnNotFoundError(target.tab_name + "." + target.col_name);
        }
    }
    return target;
}

void Analyze::get_all_cols(const std::vector<std::string> &tab_names, std::vector<ColMeta> &all_cols) {
    for (auto &sel_tab_name : tab_names) {
        const auto &sel_tab_cols = sm_manager_->db_.get_table(sel_tab_name).cols;
        all_cols.insert(all_cols.end(), sel_tab_cols.begin(), sel_tab_cols.end());
    }
}

void Analyze::get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds,
                         std::vector<Condition> &conds) {
    conds.clear();
    for (auto &expr : sv_conds) {
        Condition cond;
        cond.lhs_col = {.tab_name = expr->lhs->tab_name, .col_name = expr->lhs->col_name};
        cond.op = convert_sv_comp_op(expr->op);
        if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(expr->rhs)) {
            cond.is_rhs_val = true;
            cond.rhs_val = convert_sv_value(rhs_val);
        } else if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(expr->rhs)) {
            cond.is_rhs_val = false;
            cond.rhs_col = {.tab_name = rhs_col->tab_name, .col_name = rhs_col->col_name};
        }
        conds.push_back(cond);
    }
}

void Analyze::check_clause(const std::vector<std::string> &tab_names, std::vector<Condition> &conds) {
    std::vector<ColMeta> all_cols;
    get_all_cols(tab_names, all_cols);
    for (auto &cond : conds) {
        cond.lhs_col = check_column(all_cols, cond.lhs_col);
        if (!cond.is_rhs_val) {
            cond.rhs_col = check_column(all_cols, cond.rhs_col);
        }

        TabMeta &lhs_tab = sm_manager_->db_.get_table(cond.lhs_col.tab_name);
        auto lhs_col = lhs_tab.get_col(cond.lhs_col.col_name);
        ColType rhs_type;
        if (cond.is_rhs_val) {
            coerce_value_to_col(cond.rhs_val, *lhs_col);
            rhs_type = cond.rhs_val.type;
        } else {
            TabMeta &rhs_tab = sm_manager_->db_.get_table(cond.rhs_col.tab_name);
            auto rhs_col = rhs_tab.get_col(cond.rhs_col.col_name);
            rhs_type = rhs_col->type;
        }
        if (lhs_col->type != rhs_type) {
            throw IncompatibleTypeError(coltype2str(lhs_col->type), coltype2str(rhs_type));
        }
    }
}

Value Analyze::convert_sv_value(const std::shared_ptr<ast::Value> &sv_val) {
    Value val;
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(sv_val)) {
        errno = 0;
        char *end = nullptr;
        long long parsed = std::strtoll(int_lit->val.c_str(), &end, 10);
        if (errno == ERANGE || end == int_lit->val.c_str() || *end != '\0') {
            throw InternalError("BIGINT literal out of range");
        }
        if (parsed >= INT_MIN && parsed <= INT_MAX) {
            val.set_int(static_cast<int>(parsed));
        } else {
            val.set_bigint(static_cast<int64_t>(parsed));
        }
    } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(sv_val)) {
        val.set_float(float_lit->val);
    } else if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(sv_val)) {
        val.set_str(str_lit->val);
    } else {
        throw InternalError("Unexpected sv value type");
    }
    return val;
}

CompOp Analyze::convert_sv_comp_op(ast::SvCompOp op) {
    std::map<ast::SvCompOp, CompOp> m = {
        {ast::SV_OP_EQ, OP_EQ}, {ast::SV_OP_NE, OP_NE}, {ast::SV_OP_LT, OP_LT},
        {ast::SV_OP_GT, OP_GT}, {ast::SV_OP_LE, OP_LE}, {ast::SV_OP_GE, OP_GE},
    };
    return m.at(op);
}
