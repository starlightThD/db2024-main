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

#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "parser/parser.h"
#include "system/sm.h"
#include "common/common.h"
#include "parser/ast.h"

struct SelectItem {
    bool is_agg{false};
    ast::AggType agg_type{ast::AGG_NONE};
    TabCol col;
    std::string output_name;
};

struct HavingCond {
    SelectItem lhs; // 为了支持对聚合后的式子进行运算，如 SUM(col1) > 10
    CompOp op;
    bool is_rhs_val;
    Value rhs_val;
    SelectItem rhs_col_expr;    
};

class Query{
    public:
    std::shared_ptr<ast::TreeNode> parse;
    // TODO jointree
    // where条件
    std::vector<Condition> conds;
    // 投影列
    std::vector<TabCol> cols;
    // 表名
    std::vector<std::string> tables;
    // update 的set 值
    std::vector<SetClause> set_clauses;
    //insert 的values值
    std::vector<Value> values;
    // 新增投影列，后续取代cols
    std::vector<SelectItem> select_exprs;
    // group by 列
    std::vector<TabCol> group_by_cols;
    // having 条件
    bool has_having{false};
    std::vector<std::vector<HavingCond>> having_conds;
    Query(){}

};

class Analyze
{
private:
    SmManager *sm_manager_;
public:
    Analyze(SmManager *sm_manager) : sm_manager_(sm_manager){}
    ~Analyze(){}

    std::shared_ptr<Query> do_analyze(std::shared_ptr<ast::TreeNode> root);

private:
    TabCol check_column(const std::vector<ColMeta> &all_cols, TabCol target);
    SelectItem build_select_item_from_expr(const std::shared_ptr<ast::Expr> &sv_expr, bool allow_plain_col);
    void get_select_items(const std::vector<std::shared_ptr<ast::Expr>> &sv_sel_items, std::vector<SelectItem> &sel_items);
    void get_group_bys(const std::vector<std::shared_ptr<ast::Expr>> &sv_group_bys, std::vector<TabCol> &group_by_cols);
    void get_having_clause(const std::vector<std::vector<std::shared_ptr<ast::BinaryExpr>>> &sv_having,
                           std::vector<std::vector<HavingCond>> &having_conds);
    void get_all_cols(const std::vector<std::string> &tab_names, std::vector<ColMeta> &all_cols);
    void get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds, std::vector<Condition> &conds);
    void check_clause(const std::vector<std::string> &tab_names, std::vector<Condition> &conds);
    Value convert_sv_value(const std::shared_ptr<ast::Value> &sv_val);
    CompOp convert_sv_comp_op(ast::SvCompOp op);
    bool is_comparable_type(ColType lhs_type, ColType rhs_type);
};
