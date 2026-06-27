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

#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <list>
#include <algorithm>
#include "transaction/transaction.h"

static const std::string GroupLockModeStr[10] = {"NON_LOCK", "IS", "IX", "S", "X", "SIX"};

/**
 * @brief 多粒度锁管理器：表级意向锁 + 行级 S/X 锁 + Wait-Die 死锁预防
 *
 * 核心改造：
 * 1. 完整的多粒度锁协议：IS / IX / S / X / SIX
 *    - 加行级 S 锁前，先对表加 IS 锁
 *    - 加行级 X 锁前，先对表加 IX 锁
 *    - 意向锁之间互不冲突，意向锁与全表锁按兼容性矩阵判定
 *
 * 2. Wait-Die 死锁预防：
 *    - 老事务遇到年轻事务持锁 → 等待
 *    - 年轻事务遇到老事务持锁 → abort
 *
 * 3. 锁升级优化：S→X 升级时按 Wait-Die 处理，不直接 abort
 *
 * 4. 严格 2PL：commit/abort 时由 TransactionManager 统一释放所有锁
 */
class LockManager {
public:
    enum class LockMode { INTENTION_SHARED, INTENTION_EXCLUSIVE, SHARED, EXLUCSIVE, S_IX };
    enum class GroupLockMode { NON_LOCK, IS, IX, S, X, SIX };

    class LockRequest {
    public:
        LockRequest(Transaction* txn, txn_id_t txn_id, timestamp_t ts, LockMode lock_mode)
            : txn_(txn), txn_id_(txn_id), start_ts_(ts), lock_mode_(lock_mode), granted_(false) {}
        Transaction* txn_;
        txn_id_t txn_id_;
        timestamp_t start_ts_;
        LockMode lock_mode_;
        bool granted_;
    };

    class LockRequestQueue {
    public:
        std::list<LockRequest> request_queue_;
        std::condition_variable cv_;
        GroupLockMode group_lock_mode_ = GroupLockMode::NON_LOCK;
    };

public:
    LockManager() {}
    ~LockManager() {}

    bool lock_shared_on_table(Transaction* txn, int tab_fd);
    bool lock_exclusive_on_table(Transaction* txn, int tab_fd);

    bool lock_IS_on_table(Transaction* txn, int tab_fd);
    bool lock_IX_on_table(Transaction* txn, int tab_fd);

    bool lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd);
    bool lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd);

    bool unlock(Transaction* txn, LockDataId lock_data_id);

    void global_lock()   { /* no-op */ }
    void global_unlock() { /* no-op */ }

private:
    bool lock_internal(Transaction* txn, LockDataId id, LockMode mode);
    bool is_compatible(LockMode request_mode, GroupLockMode current_mode);
    GroupLockMode to_group_mode(LockMode mode);
    void recompute_group_mode(LockRequestQueue& q);
    bool check_wait_die(Transaction* txn, LockRequestQueue& q, LockMode request_mode);

    std::mutex latch_;
    std::unordered_map<LockDataId, LockRequestQueue> lock_table_;
};
