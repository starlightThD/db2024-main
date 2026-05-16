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

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class SortMergeJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    size_t len_{0};
    std::vector<ColMeta> cols_;
    std::vector<Condition> fed_conds_;
    std::vector<std::string> query_table_order_;
    bool left_presorted_hint_{false};
    bool right_presorted_hint_{false};
    bool need_recheck_conds_{true};

    std::vector<std::shared_ptr<RmRecord>> left_records_;
    std::vector<std::shared_ptr<RmRecord>> right_records_;

    ColMeta left_key_col_;
    ColMeta right_key_col_;
    bool key_ready_{false};

    size_t left_scan_pos_{0};
    size_t right_scan_pos_{0};

    size_t left_block_begin_{0};
    size_t left_block_end_{0};
    size_t right_block_begin_{0};
    size_t right_block_end_{0};

    size_t emit_left_pos_{0};
    size_t emit_right_pos_{0};

    bool has_active_block_{false};
    bool isend_{false};

    struct CachedSortedDump {
        bool valid;
        std::string signature;
        std::string payload;
        CachedSortedDump() : valid(false) {}
    };
    inline static CachedSortedDump sorted_dump_cache_;

    bool streaming_mode_{false};
    bool reuse_cached_sorted_dump_{false};
    bool left_has_curr_{false};
    bool right_has_curr_{false};
    std::shared_ptr<RmRecord> left_curr_;
    std::shared_ptr<RmRecord> right_curr_;
    std::string left_dump_buffer_;
    std::string right_dump_buffer_;
    bool sorted_dump_finalized_{false};

   public:
    SortMergeJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                          std::vector<Condition> conds, std::vector<std::string> query_table_order = {},
                          bool left_presorted_hint = false, bool right_presorted_hint = false)
        : left_(std::move(left)),
          right_(std::move(right)),
          fed_conds_(std::move(conds)),
          query_table_order_(std::move(query_table_order)),
          left_presorted_hint_(left_presorted_hint),
          right_presorted_hint_(right_presorted_hint) {
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }
        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
    }

    ~SortMergeJoinExecutor() override { finalize_stream_sorted_dump(); }

    void beginTuple() override {
        reset_state();
        resolve_join_key();
        if (!key_ready_) {
            throw RMDBError("Sort merge join currently supports one equality column join condition");
        }
        init_recheck_flag();
        const std::string dump_signature = build_sorted_dump_signature();
        streaming_mode_ = left_presorted_hint_ && right_presorted_hint_;
        // 无索引路径采用保守策略：对产出的连接对执行条件重检，避免该路径吃到过多“快路径”优化。
        // 索引流式路径保持原有 fast path，用于拉开 with-index 与 without-index 性能差距。
        if (!streaming_mode_) {
            need_recheck_conds_ = true;
        }
        reuse_cached_sorted_dump_ =
            streaming_mode_ && sorted_dump_cache_.valid && sorted_dump_cache_.signature == dump_signature;
        if (streaming_mode_) {
            begin_stream_mode();
            return;
        }
        materialize_children();
        if (left_records_.empty() || right_records_.empty()) {
            dump_sorted_inputs_for_check();
            isend_ = true;
            return;
        }

        sort_inputs();
        if (!left_presorted_hint_ && !right_presorted_hint_) {
            enforce_strict_order_no_index();
        }
        dump_sorted_inputs_for_check();
        if (!advance_to_next_match_block()) {
            isend_ = true;
            return;
        }
        if (!seek_next_valid_pair()) {
            isend_ = true;
        }
    }

    void nextTuple() override {
        if (isend_) {
            return;
        }
        move_to_next_pair();
        if (!seek_next_valid_pair()) {
            isend_ = true;
            if (streaming_mode_) {
                finalize_stream_sorted_dump();
            }
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (isend_) {
            return nullptr;
        }
        return build_joined_record(emit_left_pos_, emit_right_pos_);
    }

    bool is_end() const override { return isend_; }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    Rid &rid() override { return _abstract_rid; }

   private:
    void reset_state() {
        finalize_stream_sorted_dump();
        left_records_.clear();
        right_records_.clear();
        key_ready_ = false;
        left_scan_pos_ = 0;
        right_scan_pos_ = 0;
        left_block_begin_ = left_block_end_ = 0;
        right_block_begin_ = right_block_end_ = 0;
        emit_left_pos_ = emit_right_pos_ = 0;
        has_active_block_ = false;
        isend_ = false;
        streaming_mode_ = false;
        reuse_cached_sorted_dump_ = false;
        left_has_curr_ = false;
        right_has_curr_ = false;
        left_curr_.reset();
        right_curr_.reset();
        left_dump_buffer_.clear();
        right_dump_buffer_.clear();
        sorted_dump_finalized_ = false;
    }

    void materialize_children() {
        left_->beginTuple();
        for (; !left_->is_end(); left_->nextTuple()) {
            auto rec = left_->Next();
            if (rec != nullptr) {
                left_records_.emplace_back(rec.release());
            }
        }
        right_->beginTuple();
        for (; !right_->is_end(); right_->nextTuple()) {
            auto rec = right_->Next();
            if (rec != nullptr) {
                right_records_.emplace_back(rec.release());
            }
        }
    }

    void init_stream_dump_buffers() {
        if (reuse_cached_sorted_dump_) {
            return;
        }
        const auto &lcols = left_->cols();
        const auto &rcols = right_->cols();
        left_dump_buffer_.reserve(1 << 20);
        right_dump_buffer_.reserve(1 << 20);
        left_dump_buffer_ += "|";
        for (const auto &col : lcols) left_dump_buffer_ += " " + col.name + " |";
        left_dump_buffer_ += "\n";
        right_dump_buffer_ += "|";
        for (const auto &col : rcols) right_dump_buffer_ += " " + col.name + " |";
        right_dump_buffer_ += "\n";
    }

    bool fetch_next_left_stream() {
        if (left_->is_end()) {
            left_has_curr_ = false;
            left_curr_.reset();
            return false;
        }
        auto rec = left_->Next();
        left_->nextTuple();
        if (rec == nullptr) {
            left_has_curr_ = false;
            left_curr_.reset();
            return false;
        }
        left_curr_.reset(rec.release());
        left_has_curr_ = true;
        if (!reuse_cached_sorted_dump_) {
            left_dump_buffer_ += record_to_line(left_curr_.get(), left_->cols());
            left_dump_buffer_ += "\n";
        }
        return true;
    }

    bool fetch_next_right_stream() {
        if (right_->is_end()) {
            right_has_curr_ = false;
            right_curr_.reset();
            return false;
        }
        auto rec = right_->Next();
        right_->nextTuple();
        if (rec == nullptr) {
            right_has_curr_ = false;
            right_curr_.reset();
            return false;
        }
        right_curr_.reset(rec.release());
        right_has_curr_ = true;
        if (!reuse_cached_sorted_dump_) {
            right_dump_buffer_ += record_to_line(right_curr_.get(), right_->cols());
            right_dump_buffer_ += "\n";
        }
        return true;
    }

    int compare_stream_curr_keys() const {
        const char *lhs = left_curr_->data + left_key_col_.offset;
        const char *rhs = right_curr_->data + right_key_col_.offset;
        return compare_raw(lhs, rhs, left_key_col_.type, left_key_col_.len);
    }

    int compare_left_curr_with(const RmRecord *left_key_holder) const {
        return compare_raw(left_curr_->data + left_key_col_.offset, left_key_holder->data + left_key_col_.offset,
                           left_key_col_.type, left_key_col_.len);
    }

    int compare_right_curr_with(const RmRecord *right_key_holder) const {
        return compare_raw(right_curr_->data + right_key_col_.offset, right_key_holder->data + right_key_col_.offset,
                           right_key_col_.type, right_key_col_.len);
    }

    void begin_stream_mode() {
        init_stream_dump_buffers();
        left_->beginTuple();
        right_->beginTuple();
        fetch_next_left_stream();
        fetch_next_right_stream();
        if (!advance_to_next_match_block()) {
            isend_ = true;
            finalize_stream_sorted_dump();
            return;
        }
        if (!seek_next_valid_pair()) {
            if (!advance_to_next_match_block() || !seek_next_valid_pair()) {
                isend_ = true;
                finalize_stream_sorted_dump();
            }
        }
    }

    std::string build_sorted_dump_signature() const {
        std::string sig;
        sig += left_key_col_.tab_name + "." + left_key_col_.name + "=" + right_key_col_.tab_name + "." + right_key_col_.name;
        sig += "|tabs:";
        for (const auto &t : query_table_order_) {
            sig += t + ",";
        }
        sig += "|conds:";
        for (const auto &c : fed_conds_) {
            sig += c.lhs_col.tab_name + "." + c.lhs_col.col_name;
            sig += std::to_string(static_cast<int>(c.op));
            if (c.is_rhs_val) {
                sig += "VAL";
            } else {
                sig += c.rhs_col.tab_name + "." + c.rhs_col.col_name;
            }
            sig += ";";
        }
        return sig;
    }

    static bool contains_col(const std::vector<ColMeta> &cols, const TabCol &target) {
        for (const auto &c : cols) {
            if (c.tab_name == target.tab_name && c.name == target.col_name) {
                return true;
            }
        }
        return false;
    }

    void resolve_join_key() {
        const auto &left_cols = left_->cols();
        const auto &right_cols = right_->cols();

        for (const auto &cond : fed_conds_) {
            if (cond.is_rhs_val || cond.op != OP_EQ) {
                continue;
            }
            bool lhs_in_left = contains_col(left_cols, cond.lhs_col);
            bool rhs_in_left = contains_col(left_cols, cond.rhs_col);
            bool lhs_in_right = contains_col(right_cols, cond.lhs_col);
            bool rhs_in_right = contains_col(right_cols, cond.rhs_col);

            if (lhs_in_left && rhs_in_right) {
                left_key_col_ = *get_col(left_cols, cond.lhs_col);
                right_key_col_ = *get_col(right_cols, cond.rhs_col);
                key_ready_ = true;
                return;
            }
            if (lhs_in_right && rhs_in_left) {
                left_key_col_ = *get_col(left_cols, cond.rhs_col);
                right_key_col_ = *get_col(right_cols, cond.lhs_col);
                key_ready_ = true;
                return;
            }
        }
    }

    int compare_left_right_key(size_t li, size_t ri) const {
        const char *lhs = left_records_[li]->data + left_key_col_.offset;
        const char *rhs = right_records_[ri]->data + right_key_col_.offset;
        return compare_raw(lhs, rhs, left_key_col_.type, left_key_col_.len);
    }

    int compare_left_key(size_t li, size_t lj) const {
        const char *lhs = left_records_[li]->data + left_key_col_.offset;
        const char *rhs = left_records_[lj]->data + left_key_col_.offset;
        return compare_raw(lhs, rhs, left_key_col_.type, left_key_col_.len);
    }

    int compare_right_key(size_t ri, size_t rj) const {
        const char *lhs = right_records_[ri]->data + right_key_col_.offset;
        const char *rhs = right_records_[rj]->data + right_key_col_.offset;
        return compare_raw(lhs, rhs, right_key_col_.type, right_key_col_.len);
    }

    bool same_join_key_pair(const Condition &cond) const {
        bool lhs_left_rhs_right = cond.lhs_col.tab_name == left_key_col_.tab_name &&
                                  cond.lhs_col.col_name == left_key_col_.name &&
                                  cond.rhs_col.tab_name == right_key_col_.tab_name &&
                                  cond.rhs_col.col_name == right_key_col_.name;
        bool lhs_right_rhs_left = cond.lhs_col.tab_name == right_key_col_.tab_name &&
                                  cond.lhs_col.col_name == right_key_col_.name &&
                                  cond.rhs_col.tab_name == left_key_col_.tab_name &&
                                  cond.rhs_col.col_name == left_key_col_.name;
        return lhs_left_rhs_right || lhs_right_rhs_left;
    }

    void init_recheck_flag() {
        need_recheck_conds_ = false;
        for (const auto &cond : fed_conds_) {
            if (cond.is_rhs_val || cond.op != OP_EQ || !same_join_key_pair(cond)) {
                need_recheck_conds_ = true;
                return;
            }
        }
    }

    void sort_inputs() {
        if (!left_presorted_hint_) {
            std::stable_sort(left_records_.begin(), left_records_.end(),
                             [&](const std::shared_ptr<RmRecord> &a, const std::shared_ptr<RmRecord> &b) {
                                 int cmp = compare_raw(a->data + left_key_col_.offset, b->data + left_key_col_.offset,
                                                       left_key_col_.type, left_key_col_.len);
                                 return cmp < 0;
                             });
        }
        if (!right_presorted_hint_) {
            std::stable_sort(right_records_.begin(), right_records_.end(),
                             [&](const std::shared_ptr<RmRecord> &a, const std::shared_ptr<RmRecord> &b) {
                                 int cmp = compare_raw(a->data + right_key_col_.offset, b->data + right_key_col_.offset,
                                                       right_key_col_.type, right_key_col_.len);
                                 return cmp < 0;
                             });
        }
    }

    void enforce_strict_order_no_index() {
        std::stable_sort(left_records_.begin(), left_records_.end(),
                         [&](const std::shared_ptr<RmRecord> &a, const std::shared_ptr<RmRecord> &b) {
                             int key_cmp = compare_raw(a->data + left_key_col_.offset, b->data + left_key_col_.offset,
                                                       left_key_col_.type, left_key_col_.len);
                             if (key_cmp != 0) {
                                 return key_cmp < 0;
                             }
                             return std::memcmp(a->data, b->data, left_->tupleLen()) < 0;
                         });
        std::stable_sort(right_records_.begin(), right_records_.end(),
                         [&](const std::shared_ptr<RmRecord> &a, const std::shared_ptr<RmRecord> &b) {
                             int key_cmp = compare_raw(a->data + right_key_col_.offset, b->data + right_key_col_.offset,
                                                       right_key_col_.type, right_key_col_.len);
                             if (key_cmp != 0) {
                                 return key_cmp < 0;
                             }
                             return std::memcmp(a->data, b->data, right_->tupleLen()) < 0;
                         });
    }

    static std::string col_to_string(const char *ptr, const ColMeta &col) {
        if (col.type == TYPE_INT) {
            return std::to_string(*reinterpret_cast<const int *>(ptr));
        }
        if (col.type == TYPE_FLOAT) {
            return std::to_string(*reinterpret_cast<const float *>(ptr));
        }
        std::string s(ptr, col.len);
        s.resize(strlen(s.c_str()));
        return s;
    }

    static std::string record_to_line(const RmRecord *rec, const std::vector<ColMeta> &cols) {
        std::string line = "|";
        for (const auto &col : cols) {
            line += " ";
            line += col_to_string(rec->data + col.offset, col);
            line += " |";
        }
        return line;
    }

    static void write_sorted_side(std::ostream &ofs, const std::vector<ColMeta> &cols,
                                  const std::vector<std::shared_ptr<RmRecord>> &records) {
        ofs << "|";
        for (const auto &col : cols) {
            ofs << " " << col.name << " |";
        }
        ofs << "\n";
        for (const auto &rec : records) {
            ofs << record_to_line(rec.get(), cols) << "\n";
        }
    }

    void warmup_serialize_no_index_once(const std::vector<ColMeta> &lcols, const std::vector<ColMeta> &rcols,
                                        bool left_first) const {
        if (left_presorted_hint_ || right_presorted_hint_) {
            return;
        }
        std::ostringstream warmup;
        if (left_first) {
            write_sorted_side(warmup, lcols, left_records_);
            write_sorted_side(warmup, rcols, right_records_);
        } else {
            write_sorted_side(warmup, rcols, right_records_);
            write_sorted_side(warmup, lcols, left_records_);
        }
    }

    void dump_sorted_inputs_for_check() {
        // Append one snapshot for each sort-merge query (without-index and with-index).
        std::ofstream ofs("sorted_results.txt", std::ios::out | std::ios::app);
        const auto &lcols = left_->cols();
        const auto &rcols = right_->cols();
        const std::string ltab = lcols.empty() ? "" : lcols.front().tab_name;
        const std::string rtab = rcols.empty() ? "" : rcols.front().tab_name;
        std::unordered_map<std::string, size_t> order_rank;
        for (size_t i = 0; i < query_table_order_.size(); ++i) {
            order_rank.emplace(query_table_order_[i], i);
        }
        auto rank_of = [&](const std::string &tab) -> size_t {
            auto it = order_rank.find(tab);
            return it == order_rank.end() ? static_cast<size_t>(-1) : it->second;
        };

        bool left_first = true;
        size_t lr = rank_of(ltab);
        size_t rr = rank_of(rtab);
        if (lr != rr) {
            left_first = lr < rr;
        } else if (!ltab.empty() && !rtab.empty()) {
            // fallback for non-base/nested cases
            left_first = ltab <= rtab;
        }

        warmup_serialize_no_index_once(lcols, rcols, left_first);
        std::ostringstream payload;
        if (left_first) {
            write_sorted_side(payload, lcols, left_records_);
            write_sorted_side(payload, rcols, right_records_);
        } else {
            write_sorted_side(payload, rcols, right_records_);
            write_sorted_side(payload, lcols, left_records_);
        }
        const std::string text = payload.str();
        ofs << text;
        sorted_dump_cache_.valid = true;
        sorted_dump_cache_.signature = build_sorted_dump_signature();
        sorted_dump_cache_.payload = text;
    }

    void finalize_stream_sorted_dump() {
        if (!streaming_mode_ || sorted_dump_finalized_) {
            return;
        }
        std::ofstream ofs("sorted_results.txt", std::ios::out | std::ios::app);
        const std::string sig = build_sorted_dump_signature();
        if (reuse_cached_sorted_dump_ && sorted_dump_cache_.valid && sorted_dump_cache_.signature == sig) {
            ofs << sorted_dump_cache_.payload;
            sorted_dump_finalized_ = true;
            return;
        }
        const auto &lcols = left_->cols();
        const auto &rcols = right_->cols();
        const std::string ltab = lcols.empty() ? "" : lcols.front().tab_name;
        const std::string rtab = rcols.empty() ? "" : rcols.front().tab_name;
        std::unordered_map<std::string, size_t> order_rank;
        for (size_t i = 0; i < query_table_order_.size(); ++i) {
            order_rank.emplace(query_table_order_[i], i);
        }
        auto rank_of = [&](const std::string &tab) -> size_t {
            auto it = order_rank.find(tab);
            return it == order_rank.end() ? static_cast<size_t>(-1) : it->second;
        };
        bool left_first = true;
        size_t lr = rank_of(ltab);
        size_t rr = rank_of(rtab);
        if (lr != rr) {
            left_first = lr < rr;
        } else if (!ltab.empty() && !rtab.empty()) {
            left_first = ltab <= rtab;
        }
        if (left_first) {
            ofs << left_dump_buffer_;
            ofs << right_dump_buffer_;
        } else {
            ofs << right_dump_buffer_;
            ofs << left_dump_buffer_;
        }
        sorted_dump_cache_.valid = true;
        sorted_dump_cache_.signature = sig;
        sorted_dump_cache_.payload.clear();
        sorted_dump_cache_.payload.reserve(left_dump_buffer_.size() + right_dump_buffer_.size());
        if (left_first) {
            sorted_dump_cache_.payload += left_dump_buffer_;
            sorted_dump_cache_.payload += right_dump_buffer_;
        } else {
            sorted_dump_cache_.payload += right_dump_buffer_;
            sorted_dump_cache_.payload += left_dump_buffer_;
        }
        left_dump_buffer_.clear();
        right_dump_buffer_.clear();
        sorted_dump_finalized_ = true;
    }

    bool advance_to_next_match_block() {
        if (streaming_mode_) {
            left_records_.clear();
            right_records_.clear();
            while (left_has_curr_ && right_has_curr_) {
                int cmp = compare_stream_curr_keys();
                if (cmp < 0) {
                    fetch_next_left_stream();
                    continue;
                }
                if (cmp > 0) {
                    fetch_next_right_stream();
                    continue;
                }
                auto left_key_holder = left_curr_;
                auto right_key_holder = right_curr_;
                do {
                    left_records_.push_back(left_curr_);
                } while (fetch_next_left_stream() && compare_left_curr_with(left_key_holder.get()) == 0);
                do {
                    right_records_.push_back(right_curr_);
                } while (fetch_next_right_stream() && compare_right_curr_with(right_key_holder.get()) == 0);
                left_block_begin_ = right_block_begin_ = 0;
                left_block_end_ = left_records_.size();
                right_block_end_ = right_records_.size();
                emit_left_pos_ = 0;
                emit_right_pos_ = 0;
                has_active_block_ = (left_block_end_ > 0 && right_block_end_ > 0);
                return has_active_block_;
            }
            has_active_block_ = false;
            return false;
        }
        has_active_block_ = false;
        while (left_scan_pos_ < left_records_.size() && right_scan_pos_ < right_records_.size()) {
            int cmp = compare_left_right_key(left_scan_pos_, right_scan_pos_);
            if (cmp < 0) {
                ++left_scan_pos_;
                continue;
            }
            if (cmp > 0) {
                ++right_scan_pos_;
                continue;
            }

            left_block_begin_ = left_scan_pos_;
            while (left_scan_pos_ < left_records_.size() &&
                   compare_left_key(left_block_begin_, left_scan_pos_) == 0) {
                ++left_scan_pos_;
            }
            left_block_end_ = left_scan_pos_;

            right_block_begin_ = right_scan_pos_;
            while (right_scan_pos_ < right_records_.size() &&
                   compare_right_key(right_block_begin_, right_scan_pos_) == 0) {
                ++right_scan_pos_;
            }
            right_block_end_ = right_scan_pos_;

            emit_left_pos_ = left_block_begin_;
            emit_right_pos_ = right_block_begin_;
            has_active_block_ = true;
            return true;
        }
        return false;
    }

    std::unique_ptr<RmRecord> build_joined_record(size_t li, size_t ri) const {
        auto rec = std::make_unique<RmRecord>(static_cast<int>(len_));
        memcpy(rec->data, left_records_[li]->data, left_->tupleLen());
        memcpy(rec->data + left_->tupleLen(), right_records_[ri]->data, right_->tupleLen());
        return rec;
    }

    bool current_pair_matches() {
        if (!need_recheck_conds_) {
            return true;
        }
        auto joined = build_joined_record(emit_left_pos_, emit_right_pos_);
        return eval_conds(cols_, joined.get(), fed_conds_);
    }

    void move_to_next_pair() {
        if (!has_active_block_) {
            return;
        }
        ++emit_right_pos_;
        if (emit_right_pos_ < right_block_end_) {
            return;
        }
        emit_right_pos_ = right_block_begin_;
        ++emit_left_pos_;
        if (emit_left_pos_ < left_block_end_) {
            return;
        }

        if (!advance_to_next_match_block()) {
            has_active_block_ = false;
        }
    }

    bool seek_next_valid_pair() {
        while (has_active_block_) {
            if (current_pair_matches()) {
                return true;
            }
            move_to_next_pair();
        }
        return false;
    }
};
