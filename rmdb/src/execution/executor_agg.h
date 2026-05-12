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

#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "analyze/analyze.h"
#include "execution_defs.h"
#include "executor_abstract.h"
#include "optimizer/plan.h"

class AggExecutor : public AbstractExecutor {
   private:
    struct AggSpec {
        ast::AggType agg_type;
        bool has_col;
        ColMeta col_meta;
    };

    struct AggValueState {
        int count = 0;
        double sum = 0.0;
        bool min_set = false;
        bool max_set = false;
        int min_i = 0, max_i = 0;
        float min_f = 0, max_f = 0;
        std::string min_s, max_s;
    };

    struct GroupState {
        std::vector<std::shared_ptr<RmRecord>> group_vals;
        std::vector<AggValueState> agg_states;
    };

    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<SelectItem> select_exprs_;
    std::vector<TabCol> group_by_cols_;
    std::vector<std::vector<HavingCond>> having_conds_;

    std::vector<ColMeta> cols_;
    size_t len_ = 0;

    std::vector<RmRecord> out_records_;
    size_t cursor_ = 0;

    std::vector<AggSpec> agg_specs_;
    std::unordered_map<std::string, int> agg_spec_idx_;

   private:
    std::shared_ptr<RmRecord> read_col_as_record(const RmRecord *src, const ColMeta &col) const {
        auto rec = std::make_shared<RmRecord>(col.len);
        memcpy(rec->data, src->data + col.offset, col.len);
        return rec;
    }

    std::string key_for_group(const RmRecord *rec) {
        std::string key;
        for (const auto &tb_col : group_by_cols_) {
            auto it = get_col(prev_->cols(), tb_col);
            const ColMeta &cm = *it;
            key.append(rec->data + cm.offset, cm.len);
            key.push_back('\0');
        }
        return key;
    }

    static bool value_compare(const Value &a, const Value &b, CompOp op) {
        int cmp = 0;
        if (a.type == TYPE_INT && b.type == TYPE_INT) {
            cmp = (a.int_val > b.int_val) - (a.int_val < b.int_val);
        } else if (a.type == TYPE_FLOAT && b.type == TYPE_FLOAT) {
            cmp = (a.float_val > b.float_val) - (a.float_val < b.float_val);
        } else if (a.type == TYPE_STRING && b.type == TYPE_STRING) {
            cmp = a.str_val.compare(b.str_val);
        } else if (a.type == TYPE_INT && b.type == TYPE_FLOAT) {
            float av = static_cast<float>(a.int_val);
            cmp = (av > b.float_val) - (av < b.float_val);
        } else if (a.type == TYPE_FLOAT && b.type == TYPE_INT) {
            float bv = static_cast<float>(b.int_val);
            cmp = (a.float_val > bv) - (a.float_val < bv);
        } else {
            throw RMDBError("Type mismatch when evaluating having condition");
        }
        return AbstractExecutor::compare_result(cmp, op);
    }

    std::string agg_key(ast::AggType agg_type, const TabCol &col, bool has_col) const {
        if (!has_col) return std::to_string(static_cast<int>(agg_type));
        return std::to_string(static_cast<int>(agg_type)) + "|" + col.tab_name + "." + col.col_name;
    }

    int ensure_agg_spec(ast::AggType agg_type, const TabCol *col) {
        bool has_col = (agg_type != ast::AGG_COUNT_STAR);
        TabCol use_col{};
        if (has_col) use_col = *col;
        const std::string key = agg_key(agg_type, use_col, has_col);
        auto it = agg_spec_idx_.find(key);
        if (it != agg_spec_idx_.end()) return it->second;

        AggSpec spec;
        spec.agg_type = agg_type;
        spec.has_col = has_col;
        if (has_col) {
            auto col_it = get_col(prev_->cols(), use_col);
            spec.col_meta = *col_it;
        }
        int idx = static_cast<int>(agg_specs_.size());
        agg_specs_.push_back(spec);
        agg_spec_idx_[key] = idx;
        return idx;
    }

    void prepare_agg_specs() {
        agg_specs_.clear();
        agg_spec_idx_.clear();
        for (const auto &item : select_exprs_) {
            if (item.is_agg) {
                const TabCol *col_ptr = (item.agg_type == ast::AGG_COUNT_STAR ? nullptr : &item.col);
                ensure_agg_spec(item.agg_type, col_ptr);
            }
        }
        for (const auto &and_group : having_conds_) {
            for (const auto &cond : and_group) {
                if (cond.lhs.is_agg) {
                    const TabCol *col_ptr = (cond.lhs.agg_type == ast::AGG_COUNT_STAR ? nullptr : &cond.lhs.col);
                    ensure_agg_spec(cond.lhs.agg_type, col_ptr);
                }
                if (!cond.is_rhs_val && cond.rhs_col_expr.is_agg) {
                    const TabCol *col_ptr = (cond.rhs_col_expr.agg_type == ast::AGG_COUNT_STAR ? nullptr : &cond.rhs_col_expr.col);
                    ensure_agg_spec(cond.rhs_col_expr.agg_type, col_ptr);
                }
            }
        }
    }

    void update_agg_state(AggValueState &state, const AggSpec &spec, const RmRecord *rec) {
        if (spec.agg_type == ast::AGG_COUNT_STAR) {
            state.count++;
            return;
        }
        const char *ptr = rec->data + spec.col_meta.offset;
        if (spec.agg_type == ast::AGG_COUNT) {
            state.count++;
            return;
        }
        if (spec.col_meta.type == TYPE_INT) {
            int v = *reinterpret_cast<const int *>(ptr);
            if (spec.agg_type == ast::AGG_SUM || spec.agg_type == ast::AGG_AVG) state.sum += v;
            if (!state.min_set || v < state.min_i) state.min_i = v, state.min_set = true;
            if (!state.max_set || v > state.max_i) state.max_i = v, state.max_set = true;
            if (spec.agg_type == ast::AGG_AVG) state.count++;
        } else if (spec.col_meta.type == TYPE_FLOAT) {
            float v = *reinterpret_cast<const float *>(ptr);
            if (spec.agg_type == ast::AGG_SUM || spec.agg_type == ast::AGG_AVG) state.sum += v;
            if (!state.min_set || v < state.min_f) state.min_f = v, state.min_set = true;
            if (!state.max_set || v > state.max_f) state.max_f = v, state.max_set = true;
            if (spec.agg_type == ast::AGG_AVG) state.count++;
        } else if (spec.col_meta.type == TYPE_STRING) {
            std::string v(ptr, spec.col_meta.len);
            v.resize(strlen(v.c_str()));
            if (!state.min_set || v < state.min_s) state.min_s = v, state.min_set = true;
            if (!state.max_set || v > state.max_s) state.max_s = v, state.max_set = true;
        } else {
            throw RMDBError("Unsupported aggregate input type");
        }
    }

    Value value_from_nonagg(const SelectItem &item, const GroupState &group) {
        auto it = get_col(prev_->cols(), item.col);
        Value v;
        v.type = it->type;
        const char *p = group.group_vals[0]->data;
        if (it->type == TYPE_INT) {
            v.set_int(*reinterpret_cast<const int *>(p));
        } else if (it->type == TYPE_FLOAT) {
            v.set_float(*reinterpret_cast<const float *>(p));
        } else if (it->type == TYPE_STRING) {
            std::string s(p, it->len);
            s.resize(strlen(s.c_str()));
            v.set_str(s);
        }
        return v;
    }

    Value value_from_agg(const SelectItem &item, const GroupState &group) {
        const TabCol *col_ptr = (item.agg_type == ast::AGG_COUNT_STAR ? nullptr : &item.col);
        int idx = ensure_agg_spec(item.agg_type, col_ptr);
        const AggSpec &spec = agg_specs_[idx];
        const AggValueState &st = group.agg_states[idx];
        Value v;
        switch (item.agg_type) {
            case ast::AGG_COUNT:
            case ast::AGG_COUNT_STAR: {
                v.set_int(st.count);
                break;
            }
            case ast::AGG_SUM: {
                if (spec.col_meta.type == TYPE_INT) v.set_int(static_cast<int>(st.sum));
                else v.set_float(static_cast<float>(st.sum));
                break;
            }
            case ast::AGG_AVG: {
                float avg = (st.count == 0 ? 0.0f : static_cast<float>(st.sum / st.count));
                v.set_float(avg);
                break;
            }
            case ast::AGG_MIN: {
                if (spec.col_meta.type == TYPE_INT) v.set_int(st.min_i);
                else if (spec.col_meta.type == TYPE_FLOAT) v.set_float(st.min_f);
                else v.set_str(st.min_s);
                break;
            }
            case ast::AGG_MAX: {
                if (spec.col_meta.type == TYPE_INT) v.set_int(st.max_i);
                else if (spec.col_meta.type == TYPE_FLOAT) v.set_float(st.max_f);
                else v.set_str(st.max_s);
                break;
            }
            default:
                throw RMDBError("Unsupported aggregate type");
        }
        return v;
    }

    Value eval_select_item(const SelectItem &item, const GroupState &group) {
        if (item.is_agg) return value_from_agg(item, group);
        return value_from_nonagg(item, group);
    }

    bool pass_having(const GroupState &group) {
        if (having_conds_.empty()) {
            return true;
        }
        for (const auto &and_group : having_conds_) {
            bool all_true = true;
            for (const auto &cond : and_group) {
                Value lhs = eval_select_item(cond.lhs, group);
                Value rhs;
                if (cond.is_rhs_val) rhs = cond.rhs_val;
                else rhs = eval_select_item(cond.rhs_col_expr, group);
                if (!value_compare(lhs, rhs, cond.op)) {
                    all_true = false;
                    break;
                }
            }
            if (all_true) {
                return true;
            }
        }
        return false;
    }

    void build_output_meta() {
        cols_.clear();
        int offset = 0;
        for (const auto &item : select_exprs_) {
            ColMeta c;
            c.tab_name = "";
            c.name = item.output_name;
            c.offset = offset;
            c.index = false;
            if (!item.is_agg) {
                auto it = get_col(prev_->cols(), item.col);
                c.type = it->type;
                c.len = it->len;
            } else {
                if (item.agg_type == ast::AGG_COUNT || item.agg_type == ast::AGG_COUNT_STAR) {
                    c.type = TYPE_INT;
                    c.len = sizeof(int);
                } else if (item.agg_type == ast::AGG_AVG) {
                    c.type = TYPE_FLOAT;
                    c.len = sizeof(float);
                } else {
                    const TabCol *col_ptr = (item.agg_type == ast::AGG_COUNT_STAR ? nullptr : &item.col);
                    int idx = ensure_agg_spec(item.agg_type, col_ptr);
                    c.type = agg_specs_[idx].col_meta.type;
                    c.len = agg_specs_[idx].col_meta.len;
                }
            }
            cols_.push_back(c);
            offset += c.len;
        }
        len_ = offset;
    }

    void write_value(char *dest, const ColMeta &col, const Value &v) {
        if (col.type == TYPE_INT) {
            int x = (v.type == TYPE_FLOAT ? static_cast<int>(v.float_val) : v.int_val);
            memcpy(dest + col.offset, &x, sizeof(int));
        } else if (col.type == TYPE_FLOAT) {
            float x = (v.type == TYPE_INT ? static_cast<float>(v.int_val) : v.float_val);
            memcpy(dest + col.offset, &x, sizeof(float));
        } else {
            memset(dest + col.offset, 0, col.len);
            memcpy(dest + col.offset, v.str_val.c_str(), std::min(static_cast<int>(v.str_val.size()), col.len));
        }
    }

   public:
    AggExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<SelectItem> select_exprs,
                std::vector<TabCol> group_by_cols, std::vector<std::vector<HavingCond>> having_conds)
        : prev_(std::move(prev)),
          select_exprs_(std::move(select_exprs)),
          group_by_cols_(std::move(group_by_cols)),
          having_conds_(std::move(having_conds)) {
        prepare_agg_specs();
        build_output_meta();
    }

    void beginTuple() override {
        cursor_ = 0;
        out_records_.clear();

        std::unordered_map<std::string, GroupState> groups;
        prev_->beginTuple();
        for (; !prev_->is_end(); prev_->nextTuple()) {
            auto rec = prev_->Next();
            if (rec == nullptr) continue;
            std::string key = group_by_cols_.empty() ? "__all__" : key_for_group(rec.get());
            auto &state = groups[key];
            if (state.group_vals.empty()) {
                for (const auto &tb_col : group_by_cols_) {
                    auto it = get_col(prev_->cols(), tb_col);
                    state.group_vals.push_back(read_col_as_record(rec.get(), *it));
                }
                state.agg_states.resize(agg_specs_.size());
            }
            for (size_t i = 0; i < agg_specs_.size(); ++i) {
                update_agg_state(state.agg_states[i], agg_specs_[i], rec.get());
            }
        }

        if (groups.empty() && group_by_cols_.empty()) {
            GroupState state;
            state.agg_states.resize(agg_specs_.size());
            groups["__all__"] = std::move(state);
        }

        for (auto &kv : groups) {
            auto &g = kv.second;
            if (!pass_having(g)) continue;
            RmRecord rec(static_cast<int>(len_));
            for (size_t i = 0; i < select_exprs_.size(); ++i) {
                Value v = eval_select_item(select_exprs_[i], g);
                write_value(rec.data, cols_[i], v);
            }
            out_records_.push_back(rec);
        }
    }

    void nextTuple() override { ++cursor_; }

    bool is_end() const override { return cursor_ >= out_records_.size(); }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) return nullptr;
        return std::make_unique<RmRecord>(out_records_[cursor_]);
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }

    Rid &rid() override { return _abstract_rid; }
};
