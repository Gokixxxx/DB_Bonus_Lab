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
#include "transaction/txn_defs.h"

// 锁兼容性矩阵
bool LockManager::is_compatible(LockMode request_mode, GroupLockMode current_mode) {
    if (current_mode == GroupLockMode::NON_LOCK) {
        return true;
    }

    switch (request_mode) {
        case LockMode::INTENTION_SHARED:
            return current_mode == GroupLockMode::IS ||
                   current_mode == GroupLockMode::IX ||
                   current_mode == GroupLockMode::S;

        case LockMode::INTENTION_EXCLUSIVE:
            return current_mode == GroupLockMode::IS ||
                   current_mode == GroupLockMode::IX;

        case LockMode::SHARED:
            return current_mode == GroupLockMode::IS ||
                   current_mode == GroupLockMode::S;

        case LockMode::EXLUCSIVE:
            return false;

        case LockMode::S_IX:
            return current_mode == GroupLockMode::IS;

        default:
            return false;
    }
}

LockManager::GroupLockMode LockManager::to_group_mode(LockMode mode) {
    switch (mode) {
        case LockMode::INTENTION_SHARED:    return GroupLockMode::IS;
        case LockMode::INTENTION_EXCLUSIVE: return GroupLockMode::IX;
        case LockMode::SHARED:              return GroupLockMode::S;
        case LockMode::EXLUCSIVE:           return GroupLockMode::X;
        case LockMode::S_IX:                return GroupLockMode::SIX;
        default:                            return GroupLockMode::NON_LOCK;
    }
}

void LockManager::recompute_group_mode(LockRequestQueue& q) {
    GroupLockMode strongest = GroupLockMode::NON_LOCK;
    bool has_S = false;
    bool has_IX = false;

    for (const auto& req : q.request_queue_) {
        if (!req.granted_) continue;
        GroupLockMode req_mode = to_group_mode(req.lock_mode_);

        if (req_mode == GroupLockMode::X) {
            strongest = GroupLockMode::X;
            return;
        }
        if (req_mode == GroupLockMode::SIX) {
            strongest = GroupLockMode::SIX;
        } else if (req_mode == GroupLockMode::S) {
            has_S = true;
        } else if (req_mode == GroupLockMode::IX) {
            has_IX = true;
        }
    }

    if (strongest != GroupLockMode::SIX) {
        if (has_S && has_IX) {
            strongest = GroupLockMode::SIX;
        } else if (has_S) {
            strongest = GroupLockMode::S;
        } else if (has_IX) {
            strongest = GroupLockMode::IX;
        } else {
            for (const auto& req : q.request_queue_) {
                if (req.granted_ && req.lock_mode_ == LockMode::INTENTION_SHARED) {
                    strongest = GroupLockMode::IS;
                    break;
                }
            }
        }
    }

    q.group_lock_mode_ = strongest;
}

bool LockManager::lock_internal(Transaction* txn, LockDataId id, LockMode mode) {
    if (txn == nullptr) return true;
    if (txn->get_state() == TransactionState::ABORTED) {
        throw TransactionAbortException(txn->get_transaction_id(),
                                        AbortReason::DEADLOCK_PREVENTION);
    }
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(),
                                        AbortReason::LOCK_ON_SHIRINKING);
    }

    std::unique_lock<std::mutex> lk(latch_);
    auto &q = lock_table_[id];
    auto txn_id = txn->get_transaction_id();

    // 检查重复加锁或锁升级
    for (auto it = q.request_queue_.begin(); it != q.request_queue_.end(); ++it) {
        if (it->txn_id_ == txn_id) {
            if (it->granted_) {
                LockMode current_mode = it->lock_mode_;

                if (current_mode == mode) return true;
                if (current_mode == LockMode::EXLUCSIVE) return true;
                if (current_mode == LockMode::S_IX) {
                    if (mode == LockMode::SHARED || mode == LockMode::INTENTION_EXCLUSIVE ||
                        mode == LockMode::INTENTION_SHARED) {
                        return true;
                    }
                }
                if (current_mode == LockMode::SHARED &&
                    (mode == LockMode::INTENTION_SHARED)) return true;
                if (current_mode == LockMode::INTENTION_EXCLUSIVE &&
                    (mode == LockMode::INTENTION_SHARED)) return true;

                // 锁升级
                it->granted_ = false;
                recompute_group_mode(q);

                bool can_upgrade = is_compatible(mode, q.group_lock_mode_);

                if (!can_upgrade) {
                    bool can_wait = check_wait_die(txn, q, mode);
                    if (!can_wait) {
                        it->granted_ = true;
                        it->lock_mode_ = current_mode;
                        recompute_group_mode(q);
                        throw TransactionAbortException(txn_id, AbortReason::DEADLOCK_PREVENTION);
                    }
                }

                if (can_upgrade) {
                    it->lock_mode_ = mode;
                    it->granted_ = true;
                    recompute_group_mode(q);
                    return true;
                } else {
                    it->lock_mode_ = mode;

                    q.cv_.wait(lk, [txn, txn_id, &q, mode, this]() {
                        if (txn->get_state() == TransactionState::ABORTED) return true;

                        for (const auto& req : q.request_queue_) {
                            if (req.txn_id_ == txn_id) break;
                            if (req.granted_ && !is_compatible(mode, to_group_mode(req.lock_mode_))) {
                                return false;
                            }
                        }
                        return true;
                    });

                    if (txn->get_state() == TransactionState::ABORTED) {
                        for (auto it2 = q.request_queue_.begin(); it2 != q.request_queue_.end(); ++it2) {
                            if (it2->txn_id_ == txn_id) {
                                q.request_queue_.erase(it2);
                                break;
                            }
                        }
                        recompute_group_mode(q);
                        q.cv_.notify_all();
                        throw TransactionAbortException(txn_id, AbortReason::DEADLOCK_PREVENTION);
                    }

                    it->granted_ = true;
                    recompute_group_mode(q);
                    return true;
                }
            }
            return true;
        }
    }

    // 新请求
    bool can_grant = is_compatible(mode, q.group_lock_mode_);

    if (!can_grant) {
        bool can_wait = check_wait_die(txn, q, mode);
        if (!can_wait) {
            throw TransactionAbortException(txn_id, AbortReason::DEADLOCK_PREVENTION);
        }
    }

    q.request_queue_.emplace_back(txn, txn_id, txn->get_start_ts(), mode);

    if (can_grant) {
        auto &req_ref = q.request_queue_.back();
        req_ref.granted_ = true;
        recompute_group_mode(q);
        txn->get_lock_set()->insert(id);
        return true;
    }

    q.cv_.wait(lk, [txn, txn_id, &q, mode, this]() {
        if (txn->get_state() == TransactionState::ABORTED) return true;

        for (const auto& req : q.request_queue_) {
            if (req.txn_id_ == txn_id) {
                return true;
            }
            if (req.granted_ && !is_compatible(mode, to_group_mode(req.lock_mode_))) {
                return false;
            }
        }
        return false;
    });

    if (txn->get_state() == TransactionState::ABORTED) {
        for (auto it = q.request_queue_.begin(); it != q.request_queue_.end(); ++it) {
            if (it->txn_id_ == txn_id) {
                q.request_queue_.erase(it);
                break;
            }
        }
        recompute_group_mode(q);
        q.cv_.notify_all();
        throw TransactionAbortException(txn_id, AbortReason::DEADLOCK_PREVENTION);
    }

    for (auto &req : q.request_queue_) {
        if (req.txn_id_ == txn_id) {
            req.granted_ = true;
            break;
        }
    }
    recompute_group_mode(q);
    txn->get_lock_set()->insert(id);
    return true;
}

bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd) {
    if (txn == nullptr) return true;
    LockDataId id(tab_fd, LockDataType::TABLE);

    // 已持有 IX/X/SIX 锁时无需再加 S 锁，避免锁升级冲突
    {
        std::unique_lock<std::mutex> lk(latch_);
        auto it = lock_table_.find(id);
        if (it != lock_table_.end()) {
            auto& q = it->second;
            for (const auto& req : q.request_queue_) {
                if (req.txn_id_ == txn->get_transaction_id() && req.granted_) {
                    if (req.lock_mode_ == LockMode::INTENTION_EXCLUSIVE ||
                        req.lock_mode_ == LockMode::EXLUCSIVE ||
                        req.lock_mode_ == LockMode::S_IX) {
                        return true;
                    }
                    break;
                }
            }
        }
    }

    return lock_internal(txn, id, LockMode::SHARED);
}

bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd) {
    LockDataId id(tab_fd, LockDataType::TABLE);
    return lock_internal(txn, id, LockMode::EXLUCSIVE);
}

bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd) {
    if (txn == nullptr) return true;
    LockDataId id(tab_fd, LockDataType::TABLE);
    return lock_internal(txn, id, LockMode::INTENTION_SHARED);
}

bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd) {
    if (txn == nullptr) return true;
    LockDataId id(tab_fd, LockDataType::TABLE);
    return lock_internal(txn, id, LockMode::INTENTION_EXCLUSIVE);
}

bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    if (txn == nullptr) return true;
    LockDataId id(tab_fd, rid, LockDataType::RECORD);
    return lock_internal(txn, id, LockMode::SHARED);
}

bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    if (txn == nullptr) return true;
    LockDataId id(tab_fd, rid, LockDataType::RECORD);
    return lock_internal(txn, id, LockMode::EXLUCSIVE);
}

bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
    if (txn == nullptr) return true;

    std::unique_lock<std::mutex> lk(latch_);
    auto it = lock_table_.find(lock_data_id);
    if (it == lock_table_.end()) return true;
    auto &q = it->second;

    for (auto rit = q.request_queue_.begin(); rit != q.request_queue_.end(); ) {
        if (rit->txn_id_ == txn->get_transaction_id()) {
            rit = q.request_queue_.erase(rit);
        } else {
            ++rit;
        }
    }

    recompute_group_mode(q);
    q.cv_.notify_all();

    if (q.request_queue_.empty()) {
        lock_table_.erase(it);
    }

    return true;
}

// Wait-Die 死锁预防
bool LockManager::check_wait_die(Transaction* txn, LockRequestQueue& q, LockMode request_mode) {
    auto my_ts = txn->get_start_ts();
    auto txn_id = txn->get_transaction_id();

    for (const auto& req : q.request_queue_) {
        if (!req.granted_) continue;
        if (req.txn_id_ == txn_id) continue;

        bool conflict = !is_compatible(request_mode, to_group_mode(req.lock_mode_));
        if (!conflict) continue;

        // 对方比我老，我 abort
        if (req.start_ts_ < my_ts) {
            return false;
        }
    }

    return true;
}
