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

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
    }
    std::unique_ptr<RmRecord> Next() override {
        if (context_ != nullptr && context_->lock_mgr_ != nullptr && context_->txn_ != nullptr) {
            if (tab_.indexes.empty()) {
                context_->lock_mgr_->lock_exclusive_on_table(context_->txn_, fh_->GetFd());
            } else {
                context_->lock_mgr_->lock_IX_on_table(context_->txn_, fh_->GetFd());
            }
        }
        for (auto &rid : rids_) {
            if (context_ != nullptr && context_->lock_mgr_ != nullptr && context_->txn_ != nullptr) {
                context_->lock_mgr_->lock_exclusive_on_record(context_->txn_, rid, fh_->GetFd());
            }
            auto old_rec = fh_->get_record(rid, context_);
            RmRecord new_rec(fh_->get_file_hdr().record_size);
            memcpy(new_rec.data, old_rec->data, fh_->get_file_hdr().record_size);
            for (auto &set_clause : set_clauses_) {
                auto col = tab_.get_col(set_clause.lhs.col_name);
                if (col->type != set_clause.rhs.type) {
                    throw IncompatibleTypeError(coltype2str(col->type), coltype2str(set_clause.rhs.type));
                }
                if (set_clause.rhs.raw == nullptr) {
                    set_clause.rhs.init_raw(col->len);
                }
                memcpy(new_rec.data + col->offset, set_clause.rhs.raw->data, col->len);
            }
            for (auto &index : tab_.indexes) {
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                auto old_key = make_index_key(index, *old_rec);
                auto new_key = make_index_key(index, new_rec);
                if (memcmp(old_key.data(), new_key.data(), index.col_tot_len) != 0) {
                    if (context_ != nullptr && context_->lock_mgr_ != nullptr && context_->txn_ != nullptr) {
                        auto gap_lock_id = make_point_gap_lock_data_id(ih->GetFd(), index, new_key);
                        context_->lock_mgr_->lock_exclusive_on_gap(context_->txn_, gap_lock_id);
                    }
                    std::vector<Rid> existed;
                    if (ih->get_value(new_key.data(), &existed, context_->txn_) && existed[0] != rid) {
                        throw InternalError("Duplicate key violates unique index");
                    }
                    ih->delete_entry(old_key.data(), context_->txn_);
                    ih->insert_entry(new_key.data(), rid, context_->txn_);
                }
            }
            if (context_ != nullptr && context_->txn_ != nullptr) {
                context_->txn_->append_write_record(new WriteRecord(WType::UPDATE_TUPLE, tab_name_, rid, *old_rec));
            }
            fh_->update_record(rid, new_rec.data, context_);
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
