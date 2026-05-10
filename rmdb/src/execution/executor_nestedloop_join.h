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
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    size_t len_;
    std::vector<ColMeta> cols_;
    std::vector<Condition> fed_conds_;
    bool isend;
    std::unique_ptr<RmRecord> left_rec_;
    std::unique_ptr<RmRecord> right_rec_;
    std::vector<std::unique_ptr<RmRecord>> right_records_;
    int right_pos_;

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                           std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }
        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        fed_conds_ = std::move(conds);
        isend = false;
    }

    void beginTuple() override {
        isend = false;
        right_records_.clear();
        right_->beginTuple();
        for (; !right_->is_end(); right_->nextTuple()) {
            right_records_.push_back(right_->Next());
        }
        if (right_records_.empty()) {
            isend = true;
            return;
        }
        left_->beginTuple();
        if (left_->is_end()) {
            isend = true;
            return;
        }
        left_rec_ = left_->Next();
        right_pos_ = static_cast<int>(right_records_.size()) - 1;
        advance_to_match();
    }

    void nextTuple() override {
        if (isend) {
            return;
        }
        right_pos_--;
        advance_to_match();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (isend || left_rec_ == nullptr || right_rec_ == nullptr) {
            return nullptr;
        }
        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, left_rec_->data, left_->tupleLen());
        memcpy(rec->data + left_->tupleLen(), right_rec_->data, right_->tupleLen());
        return rec;
    }

    bool is_end() const override { return isend; }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    Rid &rid() override { return _abstract_rid; }

   private:
    void advance_to_match() {
        while (true) {
            while (right_pos_ >= 0) {
                right_rec_ = std::make_unique<RmRecord>(*right_records_[right_pos_]);
                auto joined = std::make_unique<RmRecord>(len_);
                memcpy(joined->data, left_rec_->data, left_->tupleLen());
                memcpy(joined->data + left_->tupleLen(), right_rec_->data, right_->tupleLen());
                if (eval_conds(cols_, joined.get(), fed_conds_)) {
                    return;
                }
                right_pos_--;
            }
            left_->nextTuple();
            if (left_->is_end()) {
                isend = true;
                return;
            }
            left_rec_ = left_->Next();
            right_pos_ = static_cast<int>(right_records_.size()) - 1;
        }
    }
};
