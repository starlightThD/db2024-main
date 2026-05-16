/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <limits>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<ColMeta> cols_;
    size_t len_;

    std::vector<std::string> index_col_names_;
    IndexMeta index_meta_;
    IxIndexHandle *ih_;

    Rid rid_;
    std::unique_ptr<IxScan> scan_;
    bool is_end_{true};
    SmManager *sm_manager_;

    static void fill_min(char *dest, const ColMeta &col) {
        if (col.type == TYPE_INT) {
            int v = std::numeric_limits<int>::min();
            memcpy(dest, &v, sizeof(int));
        } else if (col.type == TYPE_FLOAT) {
            float v = std::numeric_limits<float>::lowest();
            memcpy(dest, &v, sizeof(float));
        } else {
            memset(dest, 0, col.len);
        }
    }

    static void fill_max(char *dest, const ColMeta &col) {
        if (col.type == TYPE_INT) {
            int v = std::numeric_limits<int>::max();
            memcpy(dest, &v, sizeof(int));
        } else if (col.type == TYPE_FLOAT) {
            float v = std::numeric_limits<float>::max();
            memcpy(dest, &v, sizeof(float));
        } else {
            memset(dest, 0xff, col.len);
        }
    }

    static void write_value(char *dest, const ColMeta &col, const Value &value) {
        memcpy(dest, value.raw->data, col.len);
    }

    void make_scan_range(Iid &lower, Iid &upper) {
        std::vector<char> low_key(index_meta_.col_tot_len);
        std::vector<char> high_key(index_meta_.col_tot_len);
        int offset = 0;
        for (auto &col : index_meta_.cols) {
            fill_min(low_key.data() + offset, col);
            fill_max(high_key.data() + offset, col);
            offset += col.len;
        }

        bool use_lower_bound = true;
        bool use_upper_bound = true;
        bool has_any_index_cond = false;
        offset = 0;
        for (auto &col : index_meta_.cols) {
            const Condition *eq = nullptr;
            const Condition *lower_cond = nullptr;
            const Condition *upper_cond = nullptr;
            for (auto &cond : conds_) {
                if (!cond.is_rhs_val || cond.lhs_col.tab_name != tab_name_ || cond.lhs_col.col_name != col.name) {
                    continue;
                }
                if (cond.op == OP_EQ) {
                    eq = &cond;
                } else if (cond.op == OP_GT || cond.op == OP_GE) {
                    if (lower_cond == nullptr) {
                        lower_cond = &cond;
                    }
                } else if (cond.op == OP_LT || cond.op == OP_LE) {
                    if (upper_cond == nullptr) {
                        upper_cond = &cond;
                    }
                }
            }

            if (eq != nullptr) {
                has_any_index_cond = true;
                write_value(low_key.data() + offset, col, eq->rhs_val);
                write_value(high_key.data() + offset, col, eq->rhs_val);
                offset += col.len;
                continue;
            }

            if (lower_cond != nullptr || upper_cond != nullptr) {
                has_any_index_cond = true;
                if (lower_cond != nullptr) {
                    write_value(low_key.data() + offset, col, lower_cond->rhs_val);
                    use_lower_bound = (lower_cond->op == OP_GE);
                    int fill_offset = offset + col.len;
                    for (size_t i = (&col - index_meta_.cols.data()) + 1; i < index_meta_.cols.size(); ++i) {
                        fill_max(low_key.data() + fill_offset, index_meta_.cols[i]);
                        fill_offset += index_meta_.cols[i].len;
                    }
                }
                if (upper_cond != nullptr) {
                    write_value(high_key.data() + offset, col, upper_cond->rhs_val);
                    use_upper_bound = (upper_cond->op == OP_LE);
                    int fill_offset = offset + col.len;
                    for (size_t i = (&col - index_meta_.cols.data()) + 1; i < index_meta_.cols.size(); ++i) {
                        fill_max(high_key.data() + fill_offset, index_meta_.cols[i]);
                        fill_offset += index_meta_.cols[i].len;
                    }
                } else {
                    use_upper_bound = true;
                }
                break;
            }
            break;
        }

        if (!has_any_index_cond) {
            lower = ih_->leaf_begin();
            upper = ih_->leaf_end();
            return;
        }
        lower = use_lower_bound ? ih_->lower_bound(low_key.data()) : ih_->upper_bound(low_key.data());
        upper = use_upper_bound ? ih_->upper_bound(high_key.data()) : ih_->lower_bound(high_key.data());
    }

    void advance_to_next_match() {
        is_end_ = true;
        while (scan_ != nullptr && !scan_->is_end()) {
            Rid candidate = scan_->rid();
            if (conds_.empty()) {
                rid_ = candidate;
                is_end_ = false;
                return;
            }
            try {
                auto rec = fh_->get_record(candidate, context_);
                if (eval_conds(cols_, rec.get(), conds_)) {
                    rid_ = candidate;
                    is_end_ = false;
                    return;
                }
            } catch (const RecordNotFoundError &) {
            } catch (const PageNotExistError &) {
            }
            scan_->next();
        }
    }

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds,
                      std::vector<std::string> index_col_names, Context *context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        index_col_names_ = std::move(index_col_names);
        index_meta_ = *(tab_.get_index_meta(index_col_names_));
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;
        ih_ = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_meta_.cols)).get();
    }

    void beginTuple() override {
        Iid lower;
        Iid upper;
        make_scan_range(lower, upper);
        scan_ = std::make_unique<IxScan>(ih_, lower, upper, sm_manager_->get_bpm());
        advance_to_next_match();
    }

    void nextTuple() override {
        if (is_end_) {
            return;
        }
        scan_->next();
        advance_to_next_match();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        return fh_->get_record(rid_, context_);
    }

    Rid &rid() override { return rid_; }

    bool is_end() const override { return is_end_; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }
};
