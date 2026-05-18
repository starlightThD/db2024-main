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

#include <condition_variable>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "transaction/transaction.h"

static const std::string GroupLockModeStr[10] = {"NON_LOCK", "IS", "IX", "S", "X", "SIX"};

class LockManager {
public:
    /* 加锁类型，包括共享锁、排他锁、意向共享锁、意向排他锁、SIX（意向排他锁+共享锁） */
    enum class LockMode { SHARED, EXLUCSIVE, INTENTION_SHARED, INTENTION_EXCLUSIVE, S_IX };

    /* 用于标识加锁队列中排他性最强的锁类型，例如加锁队列中有SHARED和EXLUSIVE两个加锁操作，则该队列的锁模式为X */
    enum class GroupLockMode { NON_LOCK, IS, IX, S, X, SIX};

    /* 事务的加锁申请 */
    class LockRequest {
    public:
        LockRequest(txn_id_t txn_id, LockMode lock_mode)
            : txn_id_(txn_id), lock_mode_(lock_mode), granted_(false), upgrade_(false), prev_lock_mode_(lock_mode) {}

        txn_id_t txn_id_;   // 申请加锁的事务ID
        LockMode lock_mode_;    // 事务申请加锁的类型
        bool granted_;          // 该事务是否已经被赋予锁
        bool upgrade_;          // 该请求是否为升级请求
        LockMode prev_lock_mode_;   // 升级前已经持有的锁类型
    };

    /* 数据项上的加锁队列 */
    class LockRequestQueue {
    public:
        std::list<LockRequest> request_queue_;  // 加锁队列
        std::condition_variable cv_;            // 条件变量，用于唤醒正在等待加锁的申请，在no-wait策略下无需使用
        GroupLockMode group_lock_mode_ = GroupLockMode::NON_LOCK;   // 加锁队列的锁模式
        std::mutex latch_;                     // 单个资源对应的锁队列互斥量
        txn_id_t upgrading_txn_id_ = INVALID_TXN_ID;   // 当前正在做升级的事务ID
    };

    /* 索引区间上的加锁申请 */
    class GapLockRequest {
    public:
        GapLockRequest(txn_id_t txn_id, const LockDataId &lock_data_id, LockMode lock_mode)
            : txn_id_(txn_id), lock_data_id_(lock_data_id), lock_mode_(lock_mode), granted_(false) {}

        txn_id_t txn_id_;             // 申请加锁的事务ID
        LockDataId lock_data_id_;     // 申请加锁的索引区间
        LockMode lock_mode_;          // 事务申请加锁的类型
        bool granted_;                // 该事务是否已经被赋予锁
    };

    /* 单个索引上的区间锁队列 */
    class GapLockRequestQueue {
    public:
        std::list<GapLockRequest> request_queue_;  // 索引区间加锁队列
        std::condition_variable cv_;               // 等待区间锁释放的条件变量
        std::mutex latch_;                         // 单个索引对应的区间锁队列互斥量
    };

public:
    LockManager() {}

    ~LockManager() {}

    bool lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd);

    bool lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd);

    bool lock_shared_on_table(Transaction* txn, int tab_fd);

    bool lock_exclusive_on_table(Transaction* txn, int tab_fd);

    bool lock_IS_on_table(Transaction* txn, int tab_fd);

    bool lock_IX_on_table(Transaction* txn, int tab_fd);

    bool lock_shared_on_gap(Transaction* txn, const LockDataId &lock_data_id);

    bool lock_exclusive_on_gap(Transaction* txn, const LockDataId &lock_data_id);

    bool unlock(Transaction* txn, LockDataId lock_data_id);

private:
    bool lock_internal(Transaction *txn, const LockDataId &lock_data_id, LockMode lock_mode);

    bool lock_gap_internal(Transaction *txn, const LockDataId &lock_data_id, LockMode lock_mode);

    std::shared_ptr<LockRequestQueue> get_lock_queue(const LockDataId &lock_data_id);

    std::shared_ptr<GapLockRequestQueue> get_gap_lock_queue(int index_fd);

    static bool is_compatible_lock(LockMode held_mode, LockMode request_mode);

    static bool is_lock_covering(LockMode held_mode, LockMode request_mode);

    static GroupLockMode calc_group_lock_mode(const LockRequestQueue &queue);

    static std::list<LockRequest>::iterator find_request(std::list<LockRequest> &requests, txn_id_t txn_id);

    static bool has_conflict_with_higher_priority(const LockRequestQueue &queue, txn_id_t requester_txn_id,
                                                  timestamp_t requester_ts, LockMode request_mode,
                                                  txn_id_t ignore_txn_id = INVALID_TXN_ID);

    static bool has_conflict_with_any(const LockRequestQueue &queue, txn_id_t requester_txn_id, LockMode request_mode,
                                      txn_id_t ignore_txn_id = INVALID_TXN_ID);

    static void erase_requests_for_txn(LockRequestQueue &queue, txn_id_t txn_id);

    bool unlock_gap(Transaction *txn, const LockDataId &lock_data_id);

    std::mutex latch_;      // 用于锁表的并发
    std::unordered_map<LockDataId, std::shared_ptr<LockRequestQueue>> lock_table_;   // 全局锁表
    std::unordered_map<int, std::shared_ptr<GapLockRequestQueue>> gap_lock_table_;   // 全局索引区间锁表
};
