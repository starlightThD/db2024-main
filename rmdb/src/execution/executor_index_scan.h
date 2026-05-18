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
    std::vector<Rid> matched_rids_;
    size_t rid_pos_{0};
    SmManager *sm_manager_;
    ScanLockMode lock_mode_;
    bool lock_acquired_{false};
    IndexGapRange scan_range_;
    bool range_use_lower_bound_{true};
    bool range_use_upper_bound_{true};

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

    static int compare_value(const ColMeta &col, const Value &lhs, const Value &rhs) {
        if (col.type == TYPE_INT) {
            int lhs_val = lhs.type == TYPE_INT ? lhs.int_val : static_cast<int>(lhs.float_val);
            int rhs_val = rhs.type == TYPE_INT ? rhs.int_val : static_cast<int>(rhs.float_val);
            return (lhs_val > rhs_val) - (lhs_val < rhs_val);
        }
        if (col.type == TYPE_FLOAT) {
            float lhs_val = lhs.type == TYPE_FLOAT ? lhs.float_val : static_cast<float>(lhs.int_val);
            float rhs_val = rhs.type == TYPE_FLOAT ? rhs.float_val : static_cast<float>(rhs.int_val);
            return (lhs_val > rhs_val) - (lhs_val < rhs_val);
        }
        std::string lhs_str = lhs.raw == nullptr ? lhs.str_val : std::string(lhs.raw->data, col.len);
        std::string rhs_str = rhs.raw == nullptr ? rhs.str_val : std::string(rhs.raw->data, col.len);
        int cmp = lhs_str.compare(rhs_str);
        return (cmp > 0) - (cmp < 0);
    }

    static bool tighter_lower_bound(const ColMeta &col, const Condition *curr, const Condition &candidate) {
        if (curr == nullptr) {
            return true;
        }
        int cmp = compare_value(col, candidate.rhs_val, curr->rhs_val);
        if (cmp > 0) {
            return true;
        }
        if (cmp < 0) {
            return false;
        }
        return candidate.op == OP_GT && curr->op == OP_GE;
    }

    static bool tighter_upper_bound(const ColMeta &col, const Condition *curr, const Condition &candidate) {
        if (curr == nullptr) {
            return true;
        }
        int cmp = compare_value(col, candidate.rhs_val, curr->rhs_val);
        if (cmp < 0) {
            return true;
        }
        if (cmp > 0) {
            return false;
        }
        return candidate.op == OP_LT && curr->op == OP_LE;
    }

    void fill_suffix_min(std::vector<char> &key, size_t col_idx, int offset) {
        for (size_t i = col_idx + 1; i < index_meta_.cols.size(); ++i) {
            fill_min(key.data() + offset, index_meta_.cols[i]);
            offset += index_meta_.cols[i].len;
        }
    }

    void fill_suffix_max(std::vector<char> &key, size_t col_idx, int offset) {
        for (size_t i = col_idx + 1; i < index_meta_.cols.size(); ++i) {
            fill_max(key.data() + offset, index_meta_.cols[i]);
            offset += index_meta_.cols[i].len;
        }
    }

    void build_scan_range_keys() {
        std::vector<char> low_key(index_meta_.col_tot_len);
        std::vector<char> high_key(index_meta_.col_tot_len);
        int offset = 0;
        for (auto &col : index_meta_.cols) {
            fill_min(low_key.data() + offset, col);
            fill_max(high_key.data() + offset, col);
            offset += col.len;
        }

        range_use_lower_bound_ = true;
        range_use_upper_bound_ = true;
        bool has_any_index_cond = false;
        offset = 0;
        scan_range_ = IndexGapRange();
        scan_range_.lower_key = low_key;
        scan_range_.upper_key = high_key;
        for (size_t col_idx = 0; col_idx < index_meta_.cols.size(); ++col_idx) {
            auto &col = index_meta_.cols[col_idx];
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
                    if (tighter_lower_bound(col, lower_cond, cond)) {
                        lower_cond = &cond;
                    }
                } else if (cond.op == OP_LT || cond.op == OP_LE) {
                    if (tighter_upper_bound(col, upper_cond, cond)) {
                        upper_cond = &cond;
                    }
                }
            }

            if (eq != nullptr) {
                has_any_index_cond = true;
                write_value(low_key.data() + offset, col, eq->rhs_val);
                write_value(high_key.data() + offset, col, eq->rhs_val);
                scan_range_.lower_inf = false;
                scan_range_.upper_inf = false;
                scan_range_.lower_closed = true;
                scan_range_.upper_closed = true;
                offset += col.len;
                continue;
            }

            if (lower_cond != nullptr || upper_cond != nullptr) {
                has_any_index_cond = true;
                if (lower_cond != nullptr) {
                    write_value(low_key.data() + offset, col, lower_cond->rhs_val);
                    range_use_lower_bound_ = (lower_cond->op == OP_GE);
                    scan_range_.lower_inf = false;
                    scan_range_.lower_closed = (lower_cond->op == OP_GE);
                    int fill_offset = offset + col.len;
                    if (lower_cond->op == OP_GE) {
                        fill_suffix_min(low_key, col_idx, fill_offset);
                    } else {
                        fill_suffix_max(low_key, col_idx, fill_offset);
                    }
                }
                if (upper_cond != nullptr) {
                    write_value(high_key.data() + offset, col, upper_cond->rhs_val);
                    range_use_upper_bound_ = (upper_cond->op == OP_LE);
                    scan_range_.upper_inf = false;
                    scan_range_.upper_closed = (upper_cond->op == OP_LE);
                    int fill_offset = offset + col.len;
                    if (upper_cond->op == OP_LE) {
                        fill_suffix_max(high_key, col_idx, fill_offset);
                    } else {
                        fill_suffix_min(high_key, col_idx, fill_offset);
                    }
                } else {
                    if (!scan_range_.upper_inf) {
                        scan_range_.upper_closed = true;
                    }
                }
                break;
            }
            break;
        }

        if (!has_any_index_cond) {
            scan_range_.lower_key = low_key;
            scan_range_.upper_key = high_key;
            scan_range_.lower_inf = true;
            scan_range_.upper_inf = true;
            scan_range_.lower_closed = false;
            scan_range_.upper_closed = false;
            return;
        }
        scan_range_.lower_key = low_key;
        scan_range_.upper_key = high_key;
    }

    void locate_scan_range(Iid &lower, Iid &upper) {
        if (scan_range_.lower_inf) {
            lower = ih_->leaf_begin();
        } else {
            lower = range_use_lower_bound_ ? ih_->lower_bound(scan_range_.lower_key.data())
                                           : ih_->upper_bound(scan_range_.lower_key.data());
        }
        if (scan_range_.upper_inf) {
            upper = ih_->leaf_end();
            return;
        }
        upper = range_use_upper_bound_ ? ih_->upper_bound(scan_range_.upper_key.data())
                                       : ih_->lower_bound(scan_range_.upper_key.data());
    }

    void make_scan_range(Iid &lower, Iid &upper) {
        build_scan_range_keys();
        locate_scan_range(lower, upper);
    }

    void collect_matches() {
        matched_rids_.clear();
        while (scan_ != nullptr && !scan_->is_end()) {
            Rid candidate = scan_->rid();
            try {
                if (context_ != nullptr && context_->lock_mgr_ != nullptr && context_->txn_ != nullptr) {
                    if (lock_mode_ == ScanLockMode::READ) {
                        context_->lock_mgr_->lock_shared_on_record(context_->txn_, candidate, fh_->GetFd());
                    } else {
                        context_->lock_mgr_->lock_exclusive_on_record(context_->txn_, candidate, fh_->GetFd());
                    }
                }
                auto rec = fh_->get_record(candidate, context_);
                if (eval_conds(cols_, rec.get(), conds_)) {
                    matched_rids_.push_back(candidate);
                }
            } catch (const RecordNotFoundError &) {
            } catch (const PageNotExistError &) {
            }
            scan_->next();
        }
        rid_pos_ = 0;
        if (!matched_rids_.empty()) {
            rid_ = matched_rids_[0];
        }
    }

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds,
                      std::vector<std::string> index_col_names, Context *context,
                      ScanLockMode lock_mode = ScanLockMode::READ) {
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
        lock_mode_ = lock_mode;
    }

    void beginTuple() override {
        if (!lock_acquired_ && context_ != nullptr && context_->lock_mgr_ != nullptr && context_->txn_ != nullptr) {
            if (lock_mode_ == ScanLockMode::READ) {
                context_->lock_mgr_->lock_IS_on_table(context_->txn_, fh_->GetFd());
            } else {
                context_->lock_mgr_->lock_IX_on_table(context_->txn_, fh_->GetFd());
            }
            build_scan_range_keys();
            auto gap_lock_id = make_gap_lock_data_id(ih_->GetFd(), index_meta_, scan_range_);
            if (lock_mode_ == ScanLockMode::READ) {
                context_->lock_mgr_->lock_shared_on_gap(context_->txn_, gap_lock_id);
            } else {
                context_->lock_mgr_->lock_exclusive_on_gap(context_->txn_, gap_lock_id);
            }
            lock_acquired_ = true;
        }
        Iid lower;
        Iid upper;
        if (!lock_acquired_) {
            build_scan_range_keys();
        }
        locate_scan_range(lower, upper);
        scan_ = std::make_unique<IxScan>(ih_, lower, upper, sm_manager_->get_bpm());
        collect_matches();
    }

    void nextTuple() override {
        if (rid_pos_ < matched_rids_.size()) {
            ++rid_pos_;
        }
        if (rid_pos_ < matched_rids_.size()) {
            rid_ = matched_rids_[rid_pos_];
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        return fh_->get_record(rid_, context_);
    }

    Rid &rid() override { return rid_; }

    bool is_end() const override { return rid_pos_ >= matched_rids_.size(); }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }
};
