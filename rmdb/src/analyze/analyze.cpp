/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "analyze.h"

#include <map>
#include <unordered_map>
#include <algorithm>
#include <functional>

std::shared_ptr<Query> Analyze::do_analyze(std::shared_ptr<ast::TreeNode> parse) {
    std::shared_ptr<Query> query = std::make_shared<Query>();
    if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(parse)) {
        query->tables = std::move(x->tabs);
        for (auto &tab_name : query->tables) {
            if (!sm_manager_->db_.is_table(tab_name)) {
                throw TableNotFoundError(tab_name);
            }
        }

        for (auto &sv_sel_col : x->cols) {
            TabCol sel_col = {.tab_name = sv_sel_col->tab_name, .col_name = sv_sel_col->col_name};
            query->cols.push_back(sel_col);
        }

        std::vector<ColMeta> all_cols;
        get_all_cols(query->tables, all_cols);
        if (query->cols.empty()) {
            for (auto &col : all_cols) {
                TabCol sel_col = {.tab_name = col.tab_name, .col_name = col.name};
                query->cols.push_back(sel_col);
            }
        } else {
            for (auto &sel_col : query->cols) {
                sel_col = check_column(all_cols, sel_col);
            }
        }
        get_select_items(x->select_exprs, query->select_exprs);
        auto get_col_type = [&](const TabCol &col) -> ColType {
            auto it = std::find_if(all_cols.begin(), all_cols.end(), [&](const ColMeta &meta) {
                return meta.tab_name == col.tab_name && meta.name == col.col_name;
            });
            if (it == all_cols.end()) {
                throw ColumnNotFoundError(col.tab_name + "." + col.col_name);
            }
            return it->type;
        };
        auto validate_agg_type = [&](const SelectItem &item, const char *clause_name) {
            if (!item.is_agg || item.agg_type == ast::AGG_COUNT_STAR) {
                return;
            }
            ColType t = get_col_type(item.col);
            if (item.agg_type == ast::AGG_COUNT) {
                if (!(t == TYPE_INT || t == TYPE_FLOAT || t == TYPE_STRING)) {
                    throw RMDBError(std::string("COUNT only supports int/float/char in ") + clause_name);
                }
                return;
            }
            // According to contest requirement, MAX/MIN/SUM/AVG only support int/float fields.
            if (!(t == TYPE_INT || t == TYPE_FLOAT)) {
                throw RMDBError(std::string("Aggregate function only supports int/float in ") + clause_name);
            }
        };
        for (auto &item : query->select_exprs) {
            if (!item.is_agg || item.agg_type != ast::AGG_COUNT_STAR) {
                item.col = check_column(all_cols, item.col);
            }
            validate_agg_type(item, "SELECT");
        }
        get_group_bys(x->group_bys, query->group_by_cols);
        for (auto &group_by_col : query->group_by_cols) {
            group_by_col = check_column(all_cols, group_by_col);
        }

        std::function<bool(const std::shared_ptr<ast::Expr> &)> contains_agg_expr =
            [&](const std::shared_ptr<ast::Expr> &expr) -> bool {
                if (expr == nullptr) {
                    return false;
                }
                if (std::dynamic_pointer_cast<ast::AggExpr>(expr) != nullptr) {
                    return true;
                }
                if (auto alias_expr = std::dynamic_pointer_cast<ast::AliasExpr>(expr)) {
                    return contains_agg_expr(alias_expr->expr);
                }
                return false;
            };

        // WHERE 子句禁止使用聚合函数
        for (const auto &cond : x->conds) {
            if (contains_agg_expr(cond->lhs) || contains_agg_expr(cond->rhs)) {
                throw RMDBError("Aggregate functions are not allowed in WHERE clause; use HAVING instead");
            }
        }

        bool has_agg_select_item = std::any_of(query->select_exprs.begin(), query->select_exprs.end(),
                                               [](const SelectItem &item) { return item.is_agg; });

        // GROUP BY 存在时，SELECT 中非聚合列必须出现在 GROUP BY 中
        if (!query->group_by_cols.empty() && x->select_exprs.empty()) {
            // SELECT * with GROUP BY is invalid because * contains non-grouped non-aggregate columns.
            throw RMDBError("SELECT non-aggregate column must appear in GROUP BY clause");
        }
        if (!query->group_by_cols.empty()) {
            for (const auto &item : query->select_exprs) {
                if (item.is_agg) {
                    continue;
                }
                bool in_group_by = std::any_of(
                    query->group_by_cols.begin(), query->group_by_cols.end(),
                    [&](const TabCol &group_col) {
                        return group_col.tab_name == item.col.tab_name && group_col.col_name == item.col.col_name;
                    });
                if (!in_group_by) {
                    throw RMDBError("SELECT non-aggregate column must appear in GROUP BY clause");
                }
            }
        }

        // 无 GROUP BY 且有聚合时，不允许混合非聚合列
        if (query->group_by_cols.empty() && has_agg_select_item) {
            for (const auto &item : query->select_exprs) {
                if (!item.is_agg) {
                    throw RMDBError("SELECT non-aggregate column must appear in GROUP BY clause");
                }
            }
        }

        query->has_having = !x->having.empty();
        if (query->has_having) {
            get_having_clause(x->having, query->having_conds);
            auto in_group_by = [&](const TabCol &col) -> bool {
                return std::any_of(query->group_by_cols.begin(), query->group_by_cols.end(),
                                   [&](const TabCol &group_col) {
                                       return group_col.tab_name == col.tab_name && group_col.col_name == col.col_name;
                                   });
            };
            for (auto &having_and_group : query->having_conds) {
                for (auto &having_cond : having_and_group) {
                    if (!having_cond.lhs.is_agg) {
                        having_cond.lhs.col = check_column(all_cols, having_cond.lhs.col);
                        if (!query->group_by_cols.empty() && !in_group_by(having_cond.lhs.col)) {
                            throw RMDBError("HAVING non-aggregate column must appear in GROUP BY clause");
                        }
                    } else if (having_cond.lhs.agg_type != ast::AGG_COUNT_STAR) {
                        // Aggregate argument column must exist, but does not need to be in GROUP BY.
                        having_cond.lhs.col = check_column(all_cols, having_cond.lhs.col);
                        validate_agg_type(having_cond.lhs, "HAVING");
                    }
                    if (!having_cond.is_rhs_val) {
                        if (!having_cond.rhs_col_expr.is_agg) {
                            having_cond.rhs_col_expr.col = check_column(all_cols, having_cond.rhs_col_expr.col);
                            if (!query->group_by_cols.empty() && !in_group_by(having_cond.rhs_col_expr.col)) {
                                throw RMDBError("HAVING non-aggregate column must appear in GROUP BY clause");
                            }
                        } else if (having_cond.rhs_col_expr.agg_type != ast::AGG_COUNT_STAR) {
                            having_cond.rhs_col_expr.col = check_column(all_cols, having_cond.rhs_col_expr.col);
                            validate_agg_type(having_cond.rhs_col_expr, "HAVING");
                        }
                    }
                }
            }
        }
        get_clause(x->conds, query->conds);
        check_clause(query->tables, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(parse)) {
        if (!sm_manager_->db_.is_table(x->tab_name)) {
            throw TableNotFoundError(x->tab_name);
        }
        TabMeta &tab = sm_manager_->db_.get_table(x->tab_name);
        for (auto &sv_set_clause : x->set_clauses) {
            auto col = tab.get_col(sv_set_clause->col_name);
            SetClause set_clause;
            set_clause.lhs = {.tab_name = x->tab_name, .col_name = sv_set_clause->col_name};
            set_clause.rhs = convert_sv_value(sv_set_clause->val);
            if (col->type == TYPE_FLOAT && set_clause.rhs.type == TYPE_INT) {
                set_clause.rhs.set_float(static_cast<float>(set_clause.rhs.int_val));
            }
            if (set_clause.rhs.type != col->type) {
                throw IncompatibleTypeError(coltype2str(col->type), coltype2str(set_clause.rhs.type));
            }
            set_clause.rhs.init_raw(col->len);
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
        bool found = false;
        for (auto &col : all_cols) {
            if (col.tab_name == target.tab_name && col.name == target.col_name) {
                found = true;
                break;
            }
        }
        if (!found) {
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

SelectItem Analyze::build_select_item_from_expr(const std::shared_ptr<ast::Expr> &sv_expr, bool allow_plain_col) {
    if (auto alias_expr = std::dynamic_pointer_cast<ast::AliasExpr>(sv_expr)) {
        SelectItem item = build_select_item_from_expr(alias_expr->expr, allow_plain_col);
        item.output_name = alias_expr->alias;
        return item;
    }

    SelectItem sel_item;
    if (auto col = std::dynamic_pointer_cast<ast::Col>(sv_expr)) {
        if (!allow_plain_col) {
            throw RMDBError("Only aggregate expression is allowed in this context");
        }
        sel_item.is_agg = false;
        sel_item.agg_type = ast::AGG_NONE;
        sel_item.col = {.tab_name = col->tab_name, .col_name = col->col_name};
        sel_item.output_name = col->col_name;
    } else if (auto agg_expr = std::dynamic_pointer_cast<ast::AggExpr>(sv_expr)) {
        sel_item.is_agg = true;
        sel_item.agg_type = agg_expr->agg_type;
        std::string agg_name;
        switch (agg_expr->agg_type) {
            case ast::AGG_SUM: agg_name = "sum"; break;
            case ast::AGG_COUNT: agg_name = "count"; break;
            case ast::AGG_AVG: agg_name = "avg"; break;
            case ast::AGG_MIN: agg_name = "min"; break;
            case ast::AGG_MAX: agg_name = "max"; break;
            case ast::AGG_COUNT_STAR: agg_name = "count_star"; break;
            default: throw RMDBError("Unsupported aggregate type");
        }
        if (agg_expr->agg_type != ast::AGG_COUNT_STAR) {
            auto col = agg_expr->col;
            sel_item.col = {.tab_name = col->tab_name, .col_name = col->col_name};
            sel_item.output_name = agg_name + "_" + col->col_name;
        } else {
            sel_item.output_name = agg_name;
        }
    } else {
        throw RMDBError("Unsupported expression type in current execution pipeline");
    }
    return sel_item;
}

void Analyze::get_select_items(const std::vector<std::shared_ptr<ast::Expr>> &sv_sel_items,
                               std::vector<SelectItem> &sel_items) {
    sel_items.clear();
    std::unordered_map<std::string, int> output_name_counter;
    for (auto &sv_sel_item : sv_sel_items) {
        SelectItem sel_item = build_select_item_from_expr(sv_sel_item, true);
        int &dup_count = output_name_counter[sel_item.output_name];
        if (dup_count > 0) {
            sel_item.output_name += "_" + std::to_string(dup_count + 1);
        }
        dup_count++;
        sel_items.push_back(sel_item);
    }
}

void Analyze::get_group_bys(const std::vector<std::shared_ptr<ast::Expr>> &sv_group_bys,
                            std::vector<TabCol> &group_by_cols) {
    group_by_cols.clear();
    for (auto &sv_group_by : sv_group_bys) {
        auto col = std::dynamic_pointer_cast<ast::Col>(sv_group_by);
        if (!col) {
            throw RMDBError("Only column is supported in group by clause in current execution pipeline");
        }
        group_by_cols.push_back({.tab_name = col->tab_name, .col_name = col->col_name});
    }
}

void Analyze::get_having_clause(const std::vector<std::vector<std::shared_ptr<ast::BinaryExpr>>> &sv_having,
                                std::vector<std::vector<HavingCond>> &having_conds) {
    having_conds.clear();
    for (const auto &and_group : sv_having) {
        std::vector<HavingCond> group_conds;
        for (const auto &sv_cond : and_group) {
            HavingCond cond;
            cond.lhs = build_select_item_from_expr(sv_cond->lhs, true);
            cond.op = convert_sv_comp_op(sv_cond->op);
            if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(sv_cond->rhs)) {
                cond.is_rhs_val = true;
                cond.rhs_val = convert_sv_value(rhs_val);
            } else {
                cond.is_rhs_val = false;
                cond.rhs_col_expr = build_select_item_from_expr(sv_cond->rhs, true);
            }
            group_conds.push_back(cond);
        }
        having_conds.push_back(group_conds);
    }
}

void Analyze::get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds,
                         std::vector<Condition> &conds) {
    conds.clear();
    for (auto &expr : sv_conds) {
        Condition cond;
        auto lhs_col = std::dynamic_pointer_cast<ast::Col>(expr->lhs);
        if (!lhs_col) {
            throw RMDBError("Only column is supported on condition lhs in current execution pipeline");
        }
        cond.lhs_col = {.tab_name = lhs_col->tab_name, .col_name = lhs_col->col_name};
        cond.op = convert_sv_comp_op(expr->op);
        if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(expr->rhs)) {
            cond.is_rhs_val = true;
            cond.rhs_val = convert_sv_value(rhs_val);
        } else if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(expr->rhs)) {
            cond.is_rhs_val = false;
            cond.rhs_col = {.tab_name = rhs_col->tab_name, .col_name = rhs_col->col_name};
        } else {
            throw RMDBError("Only value/column is supported on condition rhs in current execution pipeline");
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
        ColType lhs_type = lhs_col->type;
        ColType rhs_type;
        if (cond.is_rhs_val) {
            if (lhs_type == TYPE_FLOAT && cond.rhs_val.type == TYPE_INT) {
                cond.rhs_val.set_float(static_cast<float>(cond.rhs_val.int_val));
            }
            if (cond.rhs_val.type != lhs_type) {
                throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(cond.rhs_val.type));
            }
            cond.rhs_val.init_raw(lhs_col->len);
            rhs_type = cond.rhs_val.type;
        } else {
            TabMeta &rhs_tab = sm_manager_->db_.get_table(cond.rhs_col.tab_name);
            auto rhs_col = rhs_tab.get_col(cond.rhs_col.col_name);
            rhs_type = rhs_col->type;
        }
        if (lhs_type != rhs_type) {
            throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(rhs_type));
        }
    }
}

Value Analyze::convert_sv_value(const std::shared_ptr<ast::Value> &sv_val) {
    Value val;
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(sv_val)) {
        val.set_int(int_lit->val);
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
