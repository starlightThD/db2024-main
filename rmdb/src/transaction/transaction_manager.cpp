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

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

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
    (void)log_manager;
    if (txn == nullptr) {
        return;
    }
    txn->set_state(TransactionState::COMMITTED);
}

void TransactionManager::abort(Transaction *txn, LogManager *log_manager) {
    (void)log_manager;
    if (txn == nullptr) {
        return;
    }
    txn->set_state(TransactionState::ABORTED);
}
