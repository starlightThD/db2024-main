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
#include <cstdio>
#include <fstream>
#include <memory>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class SortExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    ColMeta sort_col_;  // 当前仅支持单列排序键
    std::vector<ColMeta> cols_;
    size_t len_{0};
    bool is_desc_{false};
    std::vector<std::unique_ptr<RmRecord>> tuples_;
    size_t cursor_{0};

    bool external_mode_{false};
    bool external_end_{true};
    std::vector<std::string> run_paths_;
    std::string merged_run_path_;
    std::ifstream merged_run_ifs_;
    std::unique_ptr<RmRecord> external_curr_;

    static constexpr uint64_t kSortMemBudgetBytes = 64ull * 1024ull * 1024ull;
    static constexpr uint64_t kSortSafetyPercent = 80;
    static constexpr uint64_t kTupleOverheadBytes = 32;

   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, TabCol sel_col, bool is_desc)
        : prev_(std::move(prev)), is_desc_(is_desc) {
        sort_col_ = prev_->get_col_offset(sel_col);
        cols_ = prev_->cols();
        len_ = prev_->tupleLen();
    }

    ~SortExecutor() override { cleanup_temp_files(); }

    void beginTuple() override {
        cleanup_temp_files();
        cursor_ = 0;
        tuples_.clear();
        external_mode_ = false;
        external_end_ = true;
        external_curr_.reset();

        prev_->beginTuple();
        std::vector<std::unique_ptr<RmRecord>> chunk;
        chunk.reserve(1024);
        uint64_t chunk_bytes = 0;
        const uint64_t budget = effective_sort_budget_bytes();

        for (; !prev_->is_end(); prev_->nextTuple()) {
            auto rec = prev_->Next();
            if (rec != nullptr) {
                chunk_bytes += tuple_mem_cost(*rec);
                chunk.push_back(std::move(rec));
                if (chunk_bytes >= budget) {
                    flush_chunk_to_run(chunk);
                    chunk_bytes = 0;
                }
            }
        }

        if (run_paths_.empty()) {
            tuples_ = std::move(chunk);
            sort_records(tuples_);
            return;
        }

        if (!chunk.empty()) {
            flush_chunk_to_run(chunk);
        }

        external_mode_ = true;
        build_merged_run_file();
        open_merged_run();
    }

    void nextTuple() override {
        if (!external_mode_) {
            if (!is_end()) {
                ++cursor_;
            }
            return;
        }
        if (!external_end_) {
            external_end_ = !read_one_record(merged_run_ifs_, external_curr_);
        }
    }

    bool is_end() const override {
        if (!external_mode_) {
            return cursor_ >= tuples_.size();
        }
        return external_end_;
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        if (!external_mode_) {
            return std::make_unique<RmRecord>(*tuples_[cursor_]);
        }
        return std::make_unique<RmRecord>(*external_curr_);
    }

    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    Rid &rid() override { return _abstract_rid; }

   private:
    uint64_t effective_sort_budget_bytes() const {
        return (kSortMemBudgetBytes * kSortSafetyPercent) / 100;
    }

    uint64_t tuple_mem_cost(const RmRecord &rec) const {
        return static_cast<uint64_t>(rec.size) + kTupleOverheadBytes;
    }

    bool less_than(const RmRecord *lhs, const RmRecord *rhs) const {
        const char *lhs_raw = lhs->data + sort_col_.offset;
        const char *rhs_raw = rhs->data + sort_col_.offset;
        int cmp = compare_raw(lhs_raw, rhs_raw, sort_col_.type, sort_col_.len);
        return is_desc_ ? (cmp > 0) : (cmp < 0);
    }

    void sort_records(std::vector<std::unique_ptr<RmRecord>> &records) const {
        std::stable_sort(records.begin(), records.end(),
                         [&](const std::unique_ptr<RmRecord> &lhs, const std::unique_ptr<RmRecord> &rhs) {
                             return less_than(lhs.get(), rhs.get());
                         });
    }

    std::string tmp_file_path(const char *prefix, size_t seq) const {
        return std::string(prefix) + std::to_string(reinterpret_cast<uintptr_t>(this)) + "_" +
               std::to_string(seq) + ".tmp";
    }

    void flush_chunk_to_run(std::vector<std::unique_ptr<RmRecord>> &chunk) {
        if (chunk.empty()) {
            return;
        }
        sort_records(chunk);
        const std::string path = tmp_file_path("sort_run_", run_paths_.size());
        std::ofstream ofs(path, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            throw RMDBError("SortExecutor failed to create run file");
        }
        for (auto &rec : chunk) {
            ofs.write(rec->data, static_cast<std::streamsize>(len_));
        }
        ofs.close();
        run_paths_.push_back(path);
        chunk.clear();
    }

    bool read_one_record(std::ifstream &ifs, std::unique_ptr<RmRecord> &out) const {
        if (!ifs.good()) {
            return false;
        }
        out = std::make_unique<RmRecord>(static_cast<int>(len_));
        ifs.read(out->data, static_cast<std::streamsize>(len_));
        if (ifs.gcount() != static_cast<std::streamsize>(len_)) {
            out.reset();
            return false;
        }
        return true;
    }

    void build_merged_run_file() {
        if (run_paths_.empty()) {
            throw RMDBError("SortExecutor missing run files for external merge");
        }
        if (run_paths_.size() == 1) {
            merged_run_path_ = run_paths_.front();
            return;
        }

        struct RunState {
            std::ifstream ifs;
            std::shared_ptr<RmRecord> rec;
            bool valid{false};
        };
        struct HeapNode {
            size_t run_idx;
            std::shared_ptr<RmRecord> rec;
        };

        auto load_next = [&](RunState &state) {
            std::unique_ptr<RmRecord> one;
            state.valid = read_one_record(state.ifs, one);
            if (state.valid) {
                state.rec.reset(one.release());
            } else {
                state.rec.reset();
            }
        };

        std::vector<RunState> states(run_paths_.size());
        for (size_t i = 0; i < run_paths_.size(); ++i) {
            states[i].ifs.open(run_paths_[i], std::ios::in | std::ios::binary);
            if (!states[i].ifs.is_open()) {
                throw RMDBError("SortExecutor failed to open run file");
            }
            load_next(states[i]);
        }

        auto cmp = [&](const HeapNode &lhs, const HeapNode &rhs) {
            return less_than(rhs.rec.get(), lhs.rec.get());
        };
        std::priority_queue<HeapNode, std::vector<HeapNode>, decltype(cmp)> heap(cmp);
        for (size_t i = 0; i < states.size(); ++i) {
            if (states[i].valid) {
                heap.push(HeapNode{i, states[i].rec});
            }
        }

        merged_run_path_ = tmp_file_path("sort_merge_", 0);
        std::ofstream out(merged_run_path_, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            throw RMDBError("SortExecutor failed to create merged run file");
        }

        while (!heap.empty()) {
            HeapNode node = heap.top();
            heap.pop();
            out.write(node.rec->data, static_cast<std::streamsize>(len_));

            RunState &state = states[node.run_idx];
            load_next(state);
            if (state.valid) {
                heap.push(HeapNode{node.run_idx, state.rec});
            }
        }
        out.close();
    }

    void open_merged_run() {
        merged_run_ifs_.open(merged_run_path_, std::ios::in | std::ios::binary);
        if (!merged_run_ifs_.is_open()) {
            throw RMDBError("SortExecutor failed to open merged run file");
        }
        external_end_ = !read_one_record(merged_run_ifs_, external_curr_);
    }

    void cleanup_temp_files() {
        if (merged_run_ifs_.is_open()) {
            merged_run_ifs_.close();
        }
        for (auto &path : run_paths_) {
            std::remove(path.c_str());
        }
        if (!merged_run_path_.empty()) {
            std::remove(merged_run_path_.c_str());
        }
        run_paths_.clear();
        merged_run_path_.clear();
    }
};
