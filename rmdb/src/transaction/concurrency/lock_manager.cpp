/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lock_manager.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace {

using LockMode = LockManager::LockMode;
using LockRequest = LockManager::LockRequest;
using LockRequestQueue = LockManager::LockRequestQueue;
using GapLockRequestQueue = LockManager::GapLockRequestQueue;

static bool request_conflict(LockMode lhs, LockMode rhs) {
    switch (lhs) {
        case LockMode::SHARED:
            return !(rhs == LockMode::SHARED || rhs == LockMode::INTENTION_SHARED);
        case LockMode::EXLUCSIVE:
            return true;
        case LockMode::INTENTION_SHARED:
            return !(rhs == LockMode::SHARED || rhs == LockMode::INTENTION_SHARED ||
                     rhs == LockMode::INTENTION_EXCLUSIVE || rhs == LockMode::S_IX);
        case LockMode::INTENTION_EXCLUSIVE:
            return !(rhs == LockMode::INTENTION_SHARED || rhs == LockMode::INTENTION_EXCLUSIVE);
        case LockMode::S_IX:
            return !(rhs == LockMode::INTENTION_SHARED);
    }
    return true;
}

static int compare_gap_key(const std::vector<char> &lhs, const std::vector<char> &rhs,
                           const std::vector<ColType> &col_types, const std::vector<int> &col_lens) {
    int offset = 0;
    for (size_t i = 0; i < col_types.size(); ++i) {
        int cmp = 0;
        if (col_types[i] == TYPE_INT) {
            int lhs_val = *reinterpret_cast<const int *>(lhs.data() + offset);
            int rhs_val = *reinterpret_cast<const int *>(rhs.data() + offset);
            cmp = (lhs_val > rhs_val) - (lhs_val < rhs_val);
        } else if (col_types[i] == TYPE_FLOAT) {
            float lhs_val = *reinterpret_cast<const float *>(lhs.data() + offset);
            float rhs_val = *reinterpret_cast<const float *>(rhs.data() + offset);
            cmp = (lhs_val > rhs_val) - (lhs_val < rhs_val);
        } else {
            cmp = memcmp(lhs.data() + offset, rhs.data() + offset, col_lens[i]);
            cmp = (cmp > 0) - (cmp < 0);
        }
        if (cmp != 0) {
            return cmp;
        }
        offset += col_lens[i];
    }
    return 0;
}

static bool lower_bound_le_upper_bound(const LockDataId &lhs, const LockDataId &rhs) {
    if (lhs.gap_lower_inf_ || rhs.gap_upper_inf_) {
        return true;
    }
    int cmp = compare_gap_key(lhs.gap_lower_key_, rhs.gap_upper_key_, lhs.gap_col_types_, lhs.gap_col_lens_);
    if (cmp < 0) {
        return true;
    }
    if (cmp > 0) {
        return false;
    }
    return lhs.gap_lower_closed_ && rhs.gap_upper_closed_;
}

static bool gap_interval_overlap(const LockDataId &lhs, const LockDataId &rhs) {
    if (lhs.fd_ != rhs.fd_) {
        return false;
    }
    return lower_bound_le_upper_bound(lhs, rhs) && lower_bound_le_upper_bound(rhs, lhs);
}

static bool lower_bound_contains(const LockDataId &container, const LockDataId &target) {
    if (container.gap_lower_inf_) {
        return true;
    }
    if (target.gap_lower_inf_) {
        return false;
    }
    int cmp = compare_gap_key(container.gap_lower_key_, target.gap_lower_key_, container.gap_col_types_,
                              container.gap_col_lens_);
    if (cmp < 0) {
        return true;
    }
    if (cmp > 0) {
        return false;
    }
    return container.gap_lower_closed_ || !target.gap_lower_closed_;
}

static bool upper_bound_contains(const LockDataId &container, const LockDataId &target) {
    if (container.gap_upper_inf_) {
        return true;
    }
    if (target.gap_upper_inf_) {
        return false;
    }
    int cmp = compare_gap_key(target.gap_upper_key_, container.gap_upper_key_, container.gap_col_types_,
                              container.gap_col_lens_);
    if (cmp < 0) {
        return true;
    }
    if (cmp > 0) {
        return false;
    }
    return container.gap_upper_closed_ || !target.gap_upper_closed_;
}

static bool gap_interval_contains(const LockDataId &container, const LockDataId &target) {
    if (container.fd_ != target.fd_) {
        return false;
    }
    return lower_bound_contains(container, target) && upper_bound_contains(container, target);
}

}  // namespace

std::shared_ptr<LockRequestQueue> LockManager::get_lock_queue(const LockDataId &lock_data_id) {
    std::lock_guard<std::mutex> guard(latch_);
    auto it = lock_table_.find(lock_data_id);
    if (it != lock_table_.end()) {
        return it->second;
    }
    auto queue = std::make_shared<LockRequestQueue>();
    lock_table_.emplace(lock_data_id, queue);
    return queue;
}

std::shared_ptr<GapLockRequestQueue> LockManager::get_gap_lock_queue(int index_fd) {
    std::lock_guard<std::mutex> guard(latch_);
    auto it = gap_lock_table_.find(index_fd);
    if (it != gap_lock_table_.end()) {
        return it->second;
    }
    auto queue = std::make_shared<GapLockRequestQueue>();
    gap_lock_table_.emplace(index_fd, queue);
    return queue;
}

bool LockManager::is_compatible_lock(LockMode held_mode, LockMode request_mode) {
    return !request_conflict(held_mode, request_mode);
}

bool LockManager::is_lock_covering(LockMode held_mode, LockMode request_mode) {
    if (held_mode == request_mode) {
        return true;
    }
    if (held_mode == LockMode::EXLUCSIVE) {
        return true;
    }
    if (held_mode == LockMode::S_IX) {
        return request_mode != LockMode::EXLUCSIVE;
    }
    if (held_mode == LockMode::SHARED && request_mode == LockMode::INTENTION_SHARED) {
        return true;
    }
    return false;
}

LockManager::GroupLockMode LockManager::calc_group_lock_mode(const LockRequestQueue &queue) {
    bool has_s = false;
    bool has_ix = false;
    bool has_is = false;
    bool has_six = false;
    for (const auto &request : queue.request_queue_) {
        if (!request.granted_) {
            continue;
        }
        switch (request.lock_mode_) {
            case LockMode::EXLUCSIVE:
                return GroupLockMode::X;
            case LockMode::S_IX:
                has_six = true;
                break;
            case LockMode::SHARED:
                has_s = true;
                break;
            case LockMode::INTENTION_EXCLUSIVE:
                has_ix = true;
                break;
            case LockMode::INTENTION_SHARED:
                has_is = true;
                break;
        }
    }
    if (has_six) {
        return GroupLockMode::SIX;
    }
    if (has_s && has_ix) {
        return GroupLockMode::SIX;
    }
    if (has_s) {
        return GroupLockMode::S;
    }
    if (has_ix) {
        return GroupLockMode::IX;
    }
    if (has_is) {
        return GroupLockMode::IS;
    }
    return GroupLockMode::NON_LOCK;
}

std::list<LockRequest>::iterator LockManager::find_request(std::list<LockRequest> &requests, txn_id_t txn_id) {
    return std::find_if(requests.begin(), requests.end(), [&](const LockRequest &request) {
        return request.txn_id_ == txn_id;
    });
}

bool LockManager::has_conflict_with_higher_priority(const LockRequestQueue &queue, txn_id_t requester_txn_id,
                                                    timestamp_t requester_ts, LockMode request_mode,
                                                    txn_id_t ignore_txn_id) {
    (void)requester_ts;
    for (const auto &request : queue.request_queue_) {
        if (request.txn_id_ == requester_txn_id || request.txn_id_ == ignore_txn_id) {
            continue;
        }
        if (request_conflict(request_mode, request.lock_mode_) && request.txn_id_ < requester_txn_id) {
            return true;
        }
    }
    return false;
}

bool LockManager::has_conflict_with_any(const LockRequestQueue &queue, txn_id_t requester_txn_id, LockMode request_mode,
                                        txn_id_t ignore_txn_id) {
    for (const auto &request : queue.request_queue_) {
        if (request.txn_id_ == requester_txn_id || request.txn_id_ == ignore_txn_id) {
            continue;
        }
        if (request_conflict(request_mode, request.lock_mode_)) {
            return true;
        }
    }
    return false;
}

void LockManager::erase_requests_for_txn(LockRequestQueue &queue, txn_id_t txn_id) {
    for (auto it = queue.request_queue_.begin(); it != queue.request_queue_.end();) {
        if (it->txn_id_ == txn_id) {
            it = queue.request_queue_.erase(it);
        } else {
            ++it;
        }
    }
}

bool LockManager::lock_internal(Transaction *txn, const LockDataId &lock_data_id, LockMode lock_mode) {
    if (txn == nullptr) {
        return false;
    }
    if (txn->get_state() != TransactionState::GROWING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }

    auto queue = get_lock_queue(lock_data_id);
    std::unique_lock<std::mutex> queue_lock(queue->latch_);

    auto waiting_it = std::find_if(queue->request_queue_.begin(), queue->request_queue_.end(),
                                   [&](const LockRequest &request) {
                                       return request.txn_id_ == txn->get_transaction_id() && !request.granted_;
                                   });
    if (waiting_it != queue->request_queue_.end()) {
        queue->cv_.wait(queue_lock, [&]() { return waiting_it->granted_; });
        return true;
    }

    auto granted_it = std::find_if(queue->request_queue_.begin(), queue->request_queue_.end(),
                                   [&](const LockRequest &request) {
                                       return request.txn_id_ == txn->get_transaction_id() && request.granted_;
                                   });

    if (granted_it != queue->request_queue_.end()) {
        if (is_lock_covering(granted_it->lock_mode_, lock_mode)) {
            return true;
        }

        if (queue->upgrading_txn_id_ != INVALID_TXN_ID && queue->upgrading_txn_id_ != txn->get_transaction_id()) {
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::UPGRADE_CONFLICT);
        }
        if (queue->upgrading_txn_id_ == INVALID_TXN_ID) {
            queue->upgrading_txn_id_ = txn->get_transaction_id();
        }

        bool has_conflict = has_conflict_with_any(*queue, txn->get_transaction_id(), lock_mode);
        if (!has_conflict) {
            granted_it->lock_mode_ = lock_mode;
            granted_it->upgrade_ = false;
            granted_it->prev_lock_mode_ = lock_mode;
            queue->group_lock_mode_ = calc_group_lock_mode(*queue);
            queue->upgrading_txn_id_ = INVALID_TXN_ID;
            txn->get_lock_set()->insert(lock_data_id);
            return true;
        }

        if (has_conflict_with_higher_priority(*queue, txn->get_transaction_id(), txn->get_transaction_id(),
                                              lock_mode)) {
            queue->upgrading_txn_id_ = INVALID_TXN_ID;
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }

        queue->request_queue_.push_back(LockRequest(txn->get_transaction_id(), lock_mode));
        auto request_it = std::prev(queue->request_queue_.end());
        request_it->granted_ = false;
        request_it->upgrade_ = true;
        request_it->prev_lock_mode_ = granted_it->lock_mode_;

        queue->cv_.wait(queue_lock, [&]() {
            for (const auto &request : queue->request_queue_) {
                if (request.txn_id_ == txn->get_transaction_id()) {
                    continue;
                }
                if (!request.granted_) {
                    continue;
                }
                if (!is_compatible_lock(lock_mode, request.lock_mode_)) {
                    return false;
                }
            }
            return true;
        });

        for (auto it = queue->request_queue_.begin(); it != queue->request_queue_.end(); ++it) {
            if (it->txn_id_ == txn->get_transaction_id() && it->granted_ && it != request_it) {
                queue->request_queue_.erase(it);
                break;
            }
        }
        request_it->granted_ = true;
        request_it->upgrade_ = false;
        request_it->prev_lock_mode_ = lock_mode;
        queue->group_lock_mode_ = calc_group_lock_mode(*queue);
        queue->upgrading_txn_id_ = INVALID_TXN_ID;
        txn->get_lock_set()->insert(lock_data_id);
        queue->cv_.notify_all();
        return true;
    }

    bool has_conflict = has_conflict_with_any(*queue, txn->get_transaction_id(), lock_mode);
    if (has_conflict && has_conflict_with_higher_priority(*queue, txn->get_transaction_id(),
                                                          txn->get_transaction_id(), lock_mode)) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    }

    if (!has_conflict) {
        queue->request_queue_.push_back(LockRequest(txn->get_transaction_id(), lock_mode));
        auto request_it = std::prev(queue->request_queue_.end());
        request_it->granted_ = true;
        request_it->upgrade_ = false;
        request_it->prev_lock_mode_ = lock_mode;
        queue->group_lock_mode_ = calc_group_lock_mode(*queue);
        txn->get_lock_set()->insert(lock_data_id);
        queue->cv_.notify_all();
        return true;
    }

    queue->request_queue_.push_back(LockRequest(txn->get_transaction_id(), lock_mode));
    auto request_it = std::prev(queue->request_queue_.end());
    request_it->granted_ = false;
    request_it->upgrade_ = false;
    request_it->prev_lock_mode_ = lock_mode;

    queue->cv_.wait(queue_lock, [&]() {
        for (const auto &request : queue->request_queue_) {
            if (request.txn_id_ == txn->get_transaction_id()) {
                continue;
            }
            if (!request.granted_) {
                continue;
            }
            if (!is_compatible_lock(lock_mode, request.lock_mode_)) {
                return false;
            }
        }
        return true;
    });

    request_it->granted_ = true;
    queue->group_lock_mode_ = calc_group_lock_mode(*queue);
    txn->get_lock_set()->insert(lock_data_id);
    queue->cv_.notify_all();
    return true;
}

bool LockManager::lock_gap_internal(Transaction *txn, const LockDataId &lock_data_id, LockMode lock_mode) {
    if (txn == nullptr) {
        return false;
    }
    if (txn->get_state() != TransactionState::GROWING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }
    if (lock_data_id.type_ != LockDataType::GAP) {
        return false;
    }

    auto queue = get_gap_lock_queue(lock_data_id.fd_);
    std::unique_lock<std::mutex> queue_lock(queue->latch_);

    for (const auto &request : queue->request_queue_) {
        if (request.txn_id_ != txn->get_transaction_id() || !request.granted_) {
            continue;
        }
        if (gap_interval_contains(request.lock_data_id_, lock_data_id) &&
            is_lock_covering(request.lock_mode_, lock_mode)) {
            txn->get_lock_set()->insert(lock_data_id);
            return true;
        }
    }

    bool has_conflict = false;
    bool has_higher_priority_conflict = false;
    for (const auto &request : queue->request_queue_) {
        if (request.txn_id_ == txn->get_transaction_id()) {
            continue;
        }
        if (!request.granted_) {
            continue;
        }
        if (!gap_interval_overlap(lock_data_id, request.lock_data_id_)) {
            continue;
        }
        if (!request_conflict(lock_mode, request.lock_mode_)) {
            continue;
        }
        has_conflict = true;
        if (request.txn_id_ < txn->get_transaction_id()) {
            has_higher_priority_conflict = true;
            break;
        }
    }
    if (has_higher_priority_conflict) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    }

    queue->request_queue_.push_back(GapLockRequest(txn->get_transaction_id(), lock_data_id, lock_mode));
    auto request_it = std::prev(queue->request_queue_.end());
    if (!has_conflict) {
        request_it->granted_ = true;
        txn->get_lock_set()->insert(lock_data_id);
        queue->cv_.notify_all();
        return true;
    }

    queue->cv_.wait(queue_lock, [&]() {
        for (const auto &request : queue->request_queue_) {
            if (request.txn_id_ == txn->get_transaction_id()) {
                continue;
            }
            if (!request.granted_) {
                continue;
            }
            if (!gap_interval_overlap(lock_data_id, request.lock_data_id_)) {
                continue;
            }
            if (request_conflict(lock_mode, request.lock_mode_)) {
                return false;
            }
        }
        return true;
    });

    request_it->granted_ = true;
    txn->get_lock_set()->insert(lock_data_id);
    queue->cv_.notify_all();
    return true;
}

/**
 * @description: 申请行级共享锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID 记录所在的表的fd
 * @param {int} tab_fd
 */
bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    return lock_internal(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::SHARED);
}

/**
 * @description: 申请行级排他锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID
 * @param {int} tab_fd 记录所在的表的fd
 */
bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    return lock_internal(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::EXLUCSIVE);
}

/**
 * @description: 申请表级读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd) {
    return lock_internal(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::SHARED);
}

/**
 * @description: 申请表级写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd) {
    return lock_internal(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::EXLUCSIVE);
}

/**
 * @description: 申请表级意向读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd) {
    return lock_internal(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::INTENTION_SHARED);
}

/**
 * @description: 申请表级意向写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd) {
    return lock_internal(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::INTENTION_EXCLUSIVE);
}

bool LockManager::lock_shared_on_gap(Transaction* txn, const LockDataId &lock_data_id) {
    return lock_gap_internal(txn, lock_data_id, LockMode::SHARED);
}

bool LockManager::lock_exclusive_on_gap(Transaction* txn, const LockDataId &lock_data_id) {
    return lock_gap_internal(txn, lock_data_id, LockMode::EXLUCSIVE);
}

bool LockManager::unlock_gap(Transaction *txn, const LockDataId &lock_data_id) {
    if (txn == nullptr) {
        return false;
    }
    std::shared_ptr<GapLockRequestQueue> queue;
    {
        std::lock_guard<std::mutex> guard(latch_);
        auto it = gap_lock_table_.find(lock_data_id.fd_);
        if (it == gap_lock_table_.end()) {
            return false;
        }
        queue = it->second;
    }

    std::unique_lock<std::mutex> queue_lock(queue->latch_);
    bool removed = false;
    for (auto it = queue->request_queue_.begin(); it != queue->request_queue_.end();) {
        if (it->txn_id_ == txn->get_transaction_id() && it->lock_data_id_ == lock_data_id) {
            it = queue->request_queue_.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    if (!removed) {
        return false;
    }
    queue_lock.unlock();

    txn->get_lock_set()->erase(lock_data_id);
    queue->cv_.notify_all();
    return true;
}

/**
 * @description: 释放锁
 * @return {bool} 返回解锁是否成功
 * @param {Transaction*} txn 要释放锁的事务对象指针
 * @param {LockDataId} lock_data_id 要释放的锁ID
 */
bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
    if (txn == nullptr) {
        return false;
    }
    if (lock_data_id.type_ == LockDataType::GAP) {
        return unlock_gap(txn, lock_data_id);
    }

    std::shared_ptr<LockRequestQueue> queue;
    {
        std::lock_guard<std::mutex> guard(latch_);
        auto it = lock_table_.find(lock_data_id);
        if (it == lock_table_.end()) {
            return false;
        }
        queue = it->second;
    }
    std::unique_lock<std::mutex> queue_lock(queue->latch_);
    bool removed = false;
    for (auto it = queue->request_queue_.begin(); it != queue->request_queue_.end();) {
        if (it->txn_id_ == txn->get_transaction_id()) {
            it = queue->request_queue_.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    if (!removed) {
        return false;
    }

    if (queue->upgrading_txn_id_ == txn->get_transaction_id()) {
        queue->upgrading_txn_id_ = INVALID_TXN_ID;
    }
    queue->group_lock_mode_ = calc_group_lock_mode(*queue);
    queue_lock.unlock();

    txn->get_lock_set()->erase(lock_data_id);
    queue->cv_.notify_all();
    return true;
}
