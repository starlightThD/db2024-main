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

   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, TabCol sel_col, bool is_desc)
        : prev_(std::move(prev)), is_desc_(is_desc) {
        sort_col_ = prev_->get_col_offset(sel_col);
        cols_ = prev_->cols();
        len_ = prev_->tupleLen();
    }

    void beginTuple() override { 
        cursor_ = 0;
        tuples_.clear();

        prev_->beginTuple();
        for (; !prev_->is_end(); prev_->nextTuple()) {
            auto rec = prev_->Next();
            if (rec != nullptr) {
                tuples_.push_back(std::move(rec));
            }
        }

        std::stable_sort(tuples_.begin(), tuples_.end(),
                         [&](const std::unique_ptr<RmRecord> &lhs, const std::unique_ptr<RmRecord> &rhs) {
                             const char *lhs_raw = lhs->data + sort_col_.offset;
                             const char *rhs_raw = rhs->data + sort_col_.offset;
                             int cmp = compare_raw(lhs_raw, rhs_raw, sort_col_.type, sort_col_.len);
                             return is_desc_ ? (cmp > 0) : (cmp < 0);
                         });
    }

    void nextTuple() override {
        if (!is_end()) {
            ++cursor_;
        }
    }

    bool is_end() const override {
        return cursor_ >= tuples_.size();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*tuples_[cursor_]);
    }

    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    Rid &rid() override { return _abstract_rid; }
};
