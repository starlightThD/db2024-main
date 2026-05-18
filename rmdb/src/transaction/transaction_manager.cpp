/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"

#include "execution/execution_defs.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

namespace {

static void release_all_locks(TransactionManager *txn_mgr, Transaction *txn) {
    if (txn_mgr == nullptr || txn == nullptr) {
        return;
    }
    auto lock_set = txn->get_lock_set();
    while (!lock_set->empty()) {
        auto it = lock_set->begin();
        LockDataId lock_data_id = *it;
        if (!txn_mgr->get_lock_manager()->unlock(txn, lock_data_id)) {
            lock_set->erase(it);
        }
    }
}

}  // namespace

Transaction *TransactionManager::begin(Transaction *txn, LogManager *log_manager) {
    (void)log_manager;
    if (txn == nullptr) {
        txn = new Transaction(next_txn_id_++);
    }
    txn->set_state(TransactionState::GROWING);
    txn->set_start_ts(next_timestamp_++);
    std::unique_lock<std::mutex> lock(latch_);
    txn_map[txn->get_transaction_id()] = txn;
    return txn;
}

void TransactionManager::commit(Transaction *txn, LogManager *log_manager) {
    if (txn == nullptr || txn->get_state() == TransactionState::COMMITTED ||
        txn->get_state() == TransactionState::ABORTED) {
        return;
    }
    auto write_set = txn->get_write_set();
    while (!write_set->empty()) {
        delete write_set->back();
        write_set->pop_back();
    }
    if (log_manager != nullptr) {
        log_manager->flush_log_to_disk();
    }
    txn->set_state(TransactionState::SHRINKING);
    release_all_locks(this, txn);
    txn->get_lock_set()->clear();
    txn->set_state(TransactionState::COMMITTED);
}

void TransactionManager::abort(Transaction *txn, LogManager *log_manager) {
    if (txn == nullptr || txn->get_state() == TransactionState::COMMITTED ||
        txn->get_state() == TransactionState::ABORTED) {
        return;
    }
    auto write_set = txn->get_write_set();
    while (!write_set->empty()) {
        WriteRecord *write_record = write_set->back();
        write_set->pop_back();

        const std::string &tab_name = write_record->GetTableName();
        RmFileHandle *fh = sm_manager_->fhs_.at(tab_name).get();
        TabMeta &tab = sm_manager_->db_.get_table(tab_name);
        Rid rid = write_record->GetRid();

        if (write_record->GetWriteType() == WType::INSERT_TUPLE) {
            auto rec = fh->get_record(rid, nullptr);
            for (auto &index : tab.indexes) {
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
                auto key = make_index_key(index, *rec);
                ih->delete_entry(key.data(), txn);
            }
            fh->delete_record(rid, nullptr);
        } else if (write_record->GetWriteType() == WType::DELETE_TUPLE) {
            RmRecord &old_rec = write_record->GetRecord();
            fh->insert_record(rid, old_rec.data);
            for (auto &index : tab.indexes) {
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
                auto key = make_index_key(index, old_rec);
                ih->insert_entry(key.data(), rid, txn);
            }
        } else if (write_record->GetWriteType() == WType::UPDATE_TUPLE) {
            auto curr_rec = fh->get_record(rid, nullptr);
            RmRecord &old_rec = write_record->GetRecord();
            for (auto &index : tab.indexes) {
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
                auto curr_key = make_index_key(index, *curr_rec);
                auto old_key = make_index_key(index, old_rec);
                if (memcmp(curr_key.data(), old_key.data(), index.col_tot_len) != 0) {
                    ih->delete_entry(curr_key.data(), txn);
                    ih->insert_entry(old_key.data(), rid, txn);
                }
            }
            fh->update_record(rid, old_rec.data, nullptr);
        }
        delete write_record;
    }
    if (log_manager != nullptr) {
        log_manager->flush_log_to_disk();
    }
    txn->set_state(TransactionState::SHRINKING);
    release_all_locks(this, txn);
    txn->get_lock_set()->clear();
    txn->set_state(TransactionState::ABORTED);
}
