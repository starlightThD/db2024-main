/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "planner.h"

#include <algorithm>
#include <memory>
#include <unordered_map>

#include "execution/executor_delete.h"
#include "execution/executor_index_scan.h"
#include "execution/executor_insert.h"
#include "execution/executor_nestedloop_join.h"
#include "execution/executor_projection.h"
#include "execution/executor_seq_scan.h"
#include "execution/executor_update.h"
#include "index/ix.h"
#include "record_printer.h"

// 目前的索引匹配规则为：完全匹配索引字段，且全部为单点查询，不会自动调整where条件的顺序
bool Planner::get_index_cols(std::string tab_name, std::vector<Condition> curr_conds, std::vector<std::string>& index_col_names) {
    index_col_names.clear();
    TabMeta& tab = sm_manager_->db_.get_table(tab_name);
    size_t best_len = 0;
    std::vector<std::string> best_index;
    for (auto &index : tab.indexes) {
        size_t matched = 0;
        for (auto &col : index.cols) {
            bool has_eq = false;
            bool has_range = false;
            for (auto &cond : curr_conds) {
                if (!cond.is_rhs_val || cond.lhs_col.tab_name != tab_name || cond.lhs_col.col_name != col.name) {
                    continue;
                }
                if (cond.op == OP_EQ) {
                    has_eq = true;
                } else if (cond.op == OP_LT || cond.op == OP_LE || cond.op == OP_GT || cond.op == OP_GE) {
                    has_range = true;
                }
            }
            if (has_eq) {
                matched++;
                continue;
            }
            if (has_range) {
                matched++;
            }
            break;
        }
        if (matched > best_len) {
            best_len = matched;
            best_index.clear();
            for (auto &col : index.cols) {
                best_index.push_back(col.name);
            }
        }
    }
    if (best_len > 0) {
        index_col_names = std::move(best_index);
        return true;
    }
    return false;
}

bool Planner::get_index_cols_for_join_key(std::string tab_name, const std::string &join_col_name,
                                          std::vector<std::string> &index_col_names) {
    index_col_names.clear();
    TabMeta &tab = sm_manager_->db_.get_table(tab_name);
    for (auto &index : tab.indexes) {
        if (!index.cols.empty() && index.cols[0].name == join_col_name) {
            for (auto &col : index.cols) {
                index_col_names.push_back(col.name);
            }
            return true;
        }
    }
    return false;
}

/**
 * @brief 表算子条件谓词生成
 *
 * @param conds 条件
 * @param tab_names 表名
 * @return std::vector<Condition>
 */
std::vector<Condition> pop_conds(std::vector<Condition> &conds, std::string tab_names) {
    // auto has_tab = [&](const std::string &tab_name) {
    //     return std::find(tab_names.begin(), tab_names.end(), tab_name) != tab_names.end();
    // };
    std::vector<Condition> solved_conds;
    auto it = conds.begin();
    while (it != conds.end()) {
        if ((tab_names.compare(it->lhs_col.tab_name) == 0 && it->is_rhs_val) || (it->lhs_col.tab_name.compare(it->rhs_col.tab_name) == 0)) {
            solved_conds.emplace_back(std::move(*it));
            it = conds.erase(it);
        } else {
            it++;
        }
    }
    return solved_conds;
}

int push_conds(Condition *cond, std::shared_ptr<Plan> plan)
{
    if(auto x = std::dynamic_pointer_cast<ScanPlan>(plan))
    {
        if(x->tab_name_.compare(cond->lhs_col.tab_name) == 0) {
            return 1;
        } else if(x->tab_name_.compare(cond->rhs_col.tab_name) == 0){
            return 2;
        } else {
            return 0;
        }
    }
    else if(auto x = std::dynamic_pointer_cast<JoinPlan>(plan))
    {
        int left_res = push_conds(cond, x->left_);
        // 条件已经下推到左子节点
        if(left_res == 3){
            return 3;
        }
        int right_res = push_conds(cond, x->right_);
        // 条件已经下推到右子节点
        if(right_res == 3){
            return 3;
        }
        // 左子节点或右子节点有一个没有匹配到条件的列
        if(left_res == 0 || right_res == 0) {
            return left_res + right_res;
        }
        // 左子节点匹配到条件的右边
        if(left_res == 2) {
            // 需要将左右两边的条件变换位置
            std::map<CompOp, CompOp> swap_op = {
                {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
            };
            std::swap(cond->lhs_col, cond->rhs_col);
            cond->op = swap_op.at(cond->op);
        }
        x->conds_.emplace_back(std::move(*cond));
        return 3;
    }
    return false;
}

std::shared_ptr<Plan> pop_scan(int *scantbl, std::string table, std::vector<std::string> &joined_tables, 
                std::vector<std::shared_ptr<Plan>> plans)
{
    for (size_t i = 0; i < plans.size(); i++) {
        auto x = std::dynamic_pointer_cast<ScanPlan>(plans[i]);
        if(x->tab_name_.compare(table) == 0)
        {
            scantbl[i] = 1;
            joined_tables.emplace_back(x->tab_name_);
            return plans[i];
        }
    }
    return nullptr;
}


std::shared_ptr<Query> Planner::logical_optimization(std::shared_ptr<Query> query, Context *context)
{
    
    //TODO 实现逻辑优化规则

    return query;
}

std::shared_ptr<Plan> Planner::physical_optimization(std::shared_ptr<Query> query, Context *context)
{
    std::shared_ptr<Plan> plan = make_one_rel(query);
    
    // 其他物理优化

    // 处理orderby
    plan = generate_sort_plan(query, std::move(plan)); 

    return plan;
}



std::shared_ptr<Plan> Planner::make_one_rel(std::shared_ptr<Query> query)
{
    auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse);
    std::vector<std::string> tables = query->tables;
    auto pick_join_tag = [&]() -> PlanTag {
        if (enable_nestedloop_join && enable_sortmerge_join) {
            // 两者都开时保持原有默认策略：优先嵌套循环
            return T_NestLoop;
        }
        if (enable_nestedloop_join) {
            return T_NestLoop;
        }
        if (enable_sortmerge_join) {
            return T_SortMerge;
        }
        throw RMDBError("No join executor selected!");
    };
    auto make_join = [&](std::shared_ptr<Plan> left, std::shared_ptr<Plan> right, std::vector<Condition> conds) {
        auto jp = std::make_shared<JoinPlan>(pick_join_tag(), std::move(left), std::move(right), std::move(conds));
        jp->query_table_order_ = tables;
        return jp;
    };
    // sort-merge 场景下，记录每个表可用于连接排序键的列，以便优先选择“按键有序”的索引扫描
    std::unordered_map<std::string, std::string> join_key_by_table;
    if (enable_sortmerge_join) {
        for (const auto &cond : query->conds) {
            if (cond.is_rhs_val || cond.op != OP_EQ) {
                continue;
            }
            if (cond.lhs_col.tab_name == cond.rhs_col.tab_name) {
                continue;
            }
            if (!cond.lhs_col.tab_name.empty() && !cond.lhs_col.col_name.empty() &&
                join_key_by_table.find(cond.lhs_col.tab_name) == join_key_by_table.end()) {
                join_key_by_table.emplace(cond.lhs_col.tab_name, cond.lhs_col.col_name);
            }
            if (!cond.rhs_col.tab_name.empty() && !cond.rhs_col.col_name.empty() &&
                join_key_by_table.find(cond.rhs_col.tab_name) == join_key_by_table.end()) {
                join_key_by_table.emplace(cond.rhs_col.tab_name, cond.rhs_col.col_name);
            }
        }
    }
    // // Scan table , 生成表算子列表tab_nodes
    std::vector<std::shared_ptr<Plan>> table_scan_executors(tables.size());
    for (size_t i = 0; i < tables.size(); i++) {
        auto curr_conds = pop_conds(query->conds, tables[i]);
        // int index_no = get_indexNo(tables[i], curr_conds);
        std::vector<std::string> index_col_names;
        bool index_exist = get_index_cols(tables[i], curr_conds, index_col_names);
        if (!index_exist && enable_sortmerge_join) {
            auto it = join_key_by_table.find(tables[i]);
            if (it != join_key_by_table.end()) {
                index_exist = get_index_cols_for_join_key(tables[i], it->second, index_col_names);
            }
        }
        if (index_exist == false) {  // 该表没有索引
            index_col_names.clear();
            table_scan_executors[i] = 
                std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, tables[i], curr_conds, index_col_names);
        } else {  // 存在索引
            table_scan_executors[i] =
                std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, tables[i], curr_conds, index_col_names);
        }
    }
    // 只有一个表，不需要join。
    if(tables.size() == 1)
    {
        return table_scan_executors[0];
    }
    // 获取where条件
    auto conds = std::move(query->conds);
    std::shared_ptr<Plan> table_join_executors;
    
    int scantbl[tables.size()];
    for(size_t i = 0; i < tables.size(); i++)
    {
        scantbl[i] = -1;
    }
    // 假设在ast中已经添加了jointree，这里需要修改的逻辑是，先处理jointree，然后再考虑剩下的部分
    if(conds.size() >= 1)
    {
        // 有连接条件

        // 根据连接条件，生成第一层join
        std::vector<std::string> joined_tables(tables.size());
        auto it = conds.begin();
        while (it != conds.end()) {
            std::shared_ptr<Plan> left , right;
            left = pop_scan(scantbl, it->lhs_col.tab_name, joined_tables, table_scan_executors);
            right = pop_scan(scantbl, it->rhs_col.tab_name, joined_tables, table_scan_executors);
            std::vector<Condition> join_conds{*it};
            // 建立join，按knob选择连接算法
            table_join_executors = make_join(std::move(left), std::move(right), std::move(join_conds));

            // table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(left), std::move(right), join_conds);
            it = conds.erase(it);
            break;
        }
        // 根据连接条件，生成第2-n层join
        it = conds.begin();
        while (it != conds.end()) {
            std::shared_ptr<Plan> left_need_to_join_executors = nullptr;
            std::shared_ptr<Plan> right_need_to_join_executors = nullptr;
            bool isneedreverse = false;
            if (std::find(joined_tables.begin(), joined_tables.end(), it->lhs_col.tab_name) == joined_tables.end()) {
                left_need_to_join_executors = pop_scan(scantbl, it->lhs_col.tab_name, joined_tables, table_scan_executors);
            }
            if (std::find(joined_tables.begin(), joined_tables.end(), it->rhs_col.tab_name) == joined_tables.end()) {
                right_need_to_join_executors = pop_scan(scantbl, it->rhs_col.tab_name, joined_tables, table_scan_executors);
                isneedreverse = true;
            } 

            if(left_need_to_join_executors != nullptr && right_need_to_join_executors != nullptr) {
                std::vector<Condition> join_conds{*it};
                std::shared_ptr<Plan> temp_join_executors = make_join(std::move(left_need_to_join_executors),
                                                                       std::move(right_need_to_join_executors),
                                                                       std::move(join_conds));
                table_join_executors = make_join(std::move(temp_join_executors), std::move(table_join_executors),
                                                 std::vector<Condition>());
            } else if(left_need_to_join_executors != nullptr || right_need_to_join_executors != nullptr) {
                if(isneedreverse) {
                    std::map<CompOp, CompOp> swap_op = {
                        {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
                    };
                    std::swap(it->lhs_col, it->rhs_col);
                    it->op = swap_op.at(it->op);
                    left_need_to_join_executors = std::move(right_need_to_join_executors);
                }
                std::vector<Condition> join_conds{*it};
                table_join_executors =
                    make_join(std::move(left_need_to_join_executors), std::move(table_join_executors),
                              std::move(join_conds));
            } else {
                push_conds(std::move(&(*it)), table_join_executors);
            }
            it = conds.erase(it);
        }
    } else {
        table_join_executors = table_scan_executors[0];
        scantbl[0] = 1;
    }

    //连接剩余表
    for (size_t i = 0; i < tables.size(); i++) {
        if(scantbl[i] == -1) {
            table_join_executors = make_join(std::move(table_join_executors), std::move(table_scan_executors[i]),
                                             std::vector<Condition>());
        }
    }

    return table_join_executors;

}


std::shared_ptr<Plan> Planner::generate_sort_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan)
{
    auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse);
    if(!x->has_sort) {
        return plan;
    }
    std::vector<std::string> tables = query->tables;
    std::vector<ColMeta> all_cols;
    for (auto &sel_tab_name : tables) {
        // 这里db_不能写成get_db(), 注意要传指针
        const auto &sel_tab_cols = sm_manager_->db_.get_table(sel_tab_name).cols;
        all_cols.insert(all_cols.end(), sel_tab_cols.begin(), sel_tab_cols.end());
    }

    TabCol sel_col = {.tab_name = x->order->cols->tab_name, .col_name = x->order->cols->col_name};
    if (sel_col.tab_name.empty()) {
        std::string matched_tab;
        for (const auto &col : all_cols) {
            if (col.name == sel_col.col_name) {
                if (!matched_tab.empty() && matched_tab != col.tab_name) {
                    throw AmbiguousColumnError(sel_col.col_name);
                }
                matched_tab = col.tab_name;
            }
        }
        if (matched_tab.empty()) {
            bool matched_alias = false;
            for (const auto &item : query->select_exprs) {
                if (!item.is_agg && item.output_name == sel_col.col_name) {
                    sel_col = item.col;
                    matched_alias = true;
                    break;
                }
            }
            if (!matched_alias) {
                throw ColumnNotFoundError(sel_col.col_name);
            }
        } else {
            sel_col.tab_name = matched_tab;
        }
    } else {
        bool found = false;
        for (const auto &col : all_cols) {
            if (col.tab_name == sel_col.tab_name && col.name == sel_col.col_name) {
                found = true;
                break;
            }
        }
        if (!found) {
            throw ColumnNotFoundError(sel_col.tab_name + "." + sel_col.col_name);
        }
    }

    // 仅在“左右输入都走索引扫描”的 sort-merge 场景下复用连接输出顺序，
    // 才跳过额外 SortPlan。这样避免无索引路径(t2)也吃到该优化。
    if (x->order->orderby_dir != ast::OrderBy_DESC) {
        if (auto jp = std::dynamic_pointer_cast<JoinPlan>(plan)) {
            if (jp->tag == T_SortMerge) {
                bool both_index_scans = false;
                auto left_scan = std::dynamic_pointer_cast<ScanPlan>(jp->left_);
                auto right_scan = std::dynamic_pointer_cast<ScanPlan>(jp->right_);
                if (left_scan != nullptr && right_scan != nullptr &&
                    left_scan->tag == T_IndexScan && right_scan->tag == T_IndexScan) {
                    both_index_scans = true;
                }
                if (!both_index_scans) {
                    return std::make_shared<SortPlan>(T_Sort, std::move(plan), sel_col,
                                                      x->order->orderby_dir == ast::OrderBy_DESC);
                }
                for (const auto &cond : jp->conds_) {
                    if (cond.is_rhs_val || cond.op != OP_EQ) {
                        continue;
                    }
                    bool lhs_match = (cond.lhs_col.tab_name == sel_col.tab_name && cond.lhs_col.col_name == sel_col.col_name);
                    bool rhs_match = (cond.rhs_col.tab_name == sel_col.tab_name && cond.rhs_col.col_name == sel_col.col_name);
                    if (lhs_match || rhs_match) {
                        return plan;
                    }
                }
            }
        }
    }

    return std::make_shared<SortPlan>(T_Sort, std::move(plan), sel_col, 
                                    x->order->orderby_dir == ast::OrderBy_DESC);
}


/**
 * @brief select plan 生成
 *
 * @param sel_cols select plan 选取的列
 * @param tab_names select plan 目标的表
 * @param conds select plan 选取条件
 */
std::shared_ptr<Plan> Planner::generate_select_plan(std::shared_ptr<Query> query, Context *context) {
    //逻辑优化
    query = logical_optimization(std::move(query), context);

    //物理优化
    std::shared_ptr<Plan> plannerRoot = physical_optimization(query, context);
    bool has_agg_expr = std::any_of(query->select_exprs.begin(), query->select_exprs.end(),
                                    [](const SelectItem &item) { return item.is_agg; });
    bool needs_agg_plan = has_agg_expr || !query->group_by_cols.empty() || query->has_having;
    if (needs_agg_plan) {
        plannerRoot = std::make_shared<AggPlan>(T_Agg, std::move(plannerRoot), query->select_exprs,
                                                query->group_by_cols, query->having_conds);
    }

    // Keep ProjectionPlan as outer node for portal/executor compatibility.
    std::vector<TabCol> proj_in_cols;
    std::vector<TabCol> proj_out_cols;
    if (needs_agg_plan) {
        proj_in_cols.reserve(query->select_exprs.size());
        proj_out_cols.reserve(query->select_exprs.size());
        for (const auto &item : query->select_exprs) {
            TabCol out_col{.tab_name = "", .col_name = item.output_name};
            proj_in_cols.push_back(out_col);
            proj_out_cols.push_back(std::move(out_col));
        }
    } else {
        proj_in_cols = query->cols;
        if (query->select_exprs.empty()) {
            proj_out_cols = query->cols;
        } else {
            proj_out_cols.reserve(query->select_exprs.size());
            for (const auto &item : query->select_exprs) {
                if (!item.is_agg) {
                    proj_out_cols.push_back(TabCol{.tab_name = "", .col_name = item.output_name});
                }
            }
        }
    }
    plannerRoot = std::make_shared<ProjectionPlan>(T_Projection, std::move(plannerRoot),
                                                   std::move(proj_in_cols), std::move(proj_out_cols));

    return plannerRoot;
}

// 生成DDL语句和DML语句的查询执行计划
std::shared_ptr<Plan> Planner::do_planner(std::shared_ptr<Query> query, Context *context)
{
    std::shared_ptr<Plan> plannerRoot;
    if (auto x = std::dynamic_pointer_cast<ast::CreateTable>(query->parse)) {
        // create table;
        std::vector<ColDef> col_defs;
        for (auto &field : x->fields) {
            if (auto sv_col_def = std::dynamic_pointer_cast<ast::ColDef>(field)) {
                ColDef col_def = {.name = sv_col_def->col_name,
                                  .type = interp_sv_type(sv_col_def->type_len->type),
                                  .len = sv_col_def->type_len->len};
                col_defs.push_back(col_def);
            } else {
                throw InternalError("Unexpected field type");
            }
        }
        plannerRoot = std::make_shared<DDLPlan>(T_CreateTable, x->tab_name, std::vector<std::string>(), col_defs);
    } else if (auto x = std::dynamic_pointer_cast<ast::DropTable>(query->parse)) {
        // drop table;
        plannerRoot = std::make_shared<DDLPlan>(T_DropTable, x->tab_name, std::vector<std::string>(), std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::CreateIndex>(query->parse)) {
        // create index;
        plannerRoot = std::make_shared<DDLPlan>(T_CreateIndex, x->tab_name, x->col_names, std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::DropIndex>(query->parse)) {
        // drop index
        plannerRoot = std::make_shared<DDLPlan>(T_DropIndex, x->tab_name, x->col_names, std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(query->parse)) {
        // insert;
        plannerRoot = std::make_shared<DMLPlan>(T_Insert, std::shared_ptr<Plan>(),  x->tab_name,  
                                                    query->values, std::vector<Condition>(), std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(query->parse)) {
        // delete;
        // 生成表扫描方式
        std::shared_ptr<Plan> table_scan_executors;
        // 只有一张表，不需要进行物理优化了
        // int index_no = get_indexNo(x->tab_name, query->conds);
        std::vector<std::string> index_col_names;
        bool index_exist = get_index_cols(x->tab_name, query->conds, index_col_names);
        
        if (index_exist == false) {  // 该表没有索引
            index_col_names.clear();
            table_scan_executors = 
                std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        } else {  // 存在索引
            table_scan_executors =
                std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        }

        plannerRoot = std::make_shared<DMLPlan>(T_Delete, table_scan_executors, x->tab_name,  
                                                std::vector<Value>(), query->conds, std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(query->parse)) {
        // update;
        // 生成表扫描方式
        std::shared_ptr<Plan> table_scan_executors;
        // 只有一张表，不需要进行物理优化了
        // int index_no = get_indexNo(x->tab_name, query->conds);
        std::vector<std::string> index_col_names;
        bool index_exist = get_index_cols(x->tab_name, query->conds, index_col_names);

        if (index_exist == false) {  // 该表没有索引
        index_col_names.clear();
            table_scan_executors = 
                std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        } else {  // 存在索引
            table_scan_executors =
                std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        }
        plannerRoot = std::make_shared<DMLPlan>(T_Update, table_scan_executors, x->tab_name,
                                                     std::vector<Value>(), query->conds, 
                                                     query->set_clauses);
    } else if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse)) {

        std::shared_ptr<plannerInfo> root = std::make_shared<plannerInfo>(x);
        // 生成select语句的查询执行计划
        std::shared_ptr<Plan> projection = generate_select_plan(std::move(query), context);
        plannerRoot = std::make_shared<DMLPlan>(T_select, projection, std::string(), std::vector<Value>(),
                                                    std::vector<Condition>(), std::vector<SetClause>());
    } else {
        throw InternalError("Unexpected AST root");
    }
    return plannerRoot;
}
