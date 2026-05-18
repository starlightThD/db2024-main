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

class SeqScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;              // 表的名称
    TabMeta tab_;                       // 表的元数据
    std::vector<Condition> conds_;      // scan的条件
    RmFileHandle *fh_;                  // 表的数据文件句柄
    std::vector<ColMeta> cols_;         // scan后生成的记录的字段
    size_t len_;                        // scan后生成的每条记录的长度
    std::vector<Condition> fed_conds_;  // 同conds_，两个字段相同

    Rid rid_;
    std::unique_ptr<RecScan> scan_;     // table_iterator

    SmManager *sm_manager_;
    ScanLockMode lock_mode_;
    bool lock_acquired_{false};
    bool use_index_gap_lock_{false};

    void lock_full_index_gap() {
        if (tab_.indexes.empty() || context_ == nullptr || context_->lock_mgr_ == nullptr || context_->txn_ == nullptr) {
            return;
        }
        auto &index = tab_.indexes.front();
        auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
        IndexGapRange range;
        auto gap_lock_id = make_gap_lock_data_id(ih->GetFd(), index, range);
        if (lock_mode_ == ScanLockMode::READ) {
            context_->lock_mgr_->lock_shared_on_gap(context_->txn_, gap_lock_id);
        } else {
            context_->lock_mgr_->lock_exclusive_on_gap(context_->txn_, gap_lock_id);
        }
        use_index_gap_lock_ = true;
    }

    void lock_current_record(const Rid &rid) {
        if (!use_index_gap_lock_ || context_ == nullptr || context_->lock_mgr_ == nullptr || context_->txn_ == nullptr) {
            return;
        }
        if (lock_mode_ == ScanLockMode::READ) {
            context_->lock_mgr_->lock_shared_on_record(context_->txn_, rid, fh_->GetFd());
        } else {
            context_->lock_mgr_->lock_exclusive_on_record(context_->txn_, rid, fh_->GetFd());
        }
    }

   public:
    SeqScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, Context *context,
                    ScanLockMode lock_mode = ScanLockMode::READ) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        conds_ = std::move(conds);
        TabMeta &tab = sm_manager_->db_.get_table(tab_name_);
        tab_ = tab;
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab.cols;
        len_ = cols_.back().offset + cols_.back().len;

        context_ = context;
        lock_mode_ = lock_mode;

        fed_conds_ = conds_;
    }

    void beginTuple() override {
        if (!lock_acquired_ && context_ != nullptr && context_->lock_mgr_ != nullptr && context_->txn_ != nullptr) {
            if (tab_.indexes.empty()) {
                if (lock_mode_ == ScanLockMode::READ) {
                    context_->lock_mgr_->lock_shared_on_table(context_->txn_, fh_->GetFd());
                } else {
                    context_->lock_mgr_->lock_exclusive_on_table(context_->txn_, fh_->GetFd());
                }
            } else {
                if (lock_mode_ == ScanLockMode::READ) {
                    context_->lock_mgr_->lock_IS_on_table(context_->txn_, fh_->GetFd());
                } else {
                    context_->lock_mgr_->lock_IX_on_table(context_->txn_, fh_->GetFd());
                }
                lock_full_index_gap();
            }
            lock_acquired_ = true;
        }
        scan_ = std::make_unique<RmScan>(fh_);
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            lock_current_record(rid_);
            auto rec = fh_->get_record(rid_, context_);
            if (eval_conds(cols_, rec.get(), fed_conds_)) {
                return;
            }
            scan_->next();
        }
    }

    void nextTuple() override {
        if (scan_ == nullptr || scan_->is_end()) {
            return;
        }
        scan_->next();
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            lock_current_record(rid_);
            auto rec = fh_->get_record(rid_, context_);
            if (eval_conds(cols_, rec.get(), fed_conds_)) {
                return;
            }
            scan_->next();
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (scan_ == nullptr || scan_->is_end()) {
            return nullptr;
        }
        rid_ = scan_->rid();
        return fh_->get_record(rid_, context_);
    }

    bool is_end() const override { return scan_ == nullptr || scan_->is_end(); }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    Rid &rid() override { return rid_; }
};
