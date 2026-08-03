/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "lock_manager.h"

bool LockManager::compatible(LockMode requested, LockMode granted) const {
    if (requested == LockMode::INTENTION_SHARED) {
        return granted != LockMode::EXLUCSIVE;
    }
    if (requested == LockMode::INTENTION_EXCLUSIVE) {
        return granted == LockMode::INTENTION_SHARED || granted == LockMode::INTENTION_EXCLUSIVE;
    }
    if (requested == LockMode::SHARED) {
        return granted == LockMode::SHARED || granted == LockMode::INTENTION_SHARED;
    }
    if (requested == LockMode::S_IX) {
        return granted == LockMode::INTENTION_SHARED;
    }
    return false;
}

void LockManager::refresh_group_lock_mode(LockRequestQueue &queue) {
    bool has_s = false;
    bool has_x = false;
    bool has_is = false;
    bool has_ix = false;
    for (auto &request : queue.request_queue_) {
        if (!request.granted_) {
            continue;
        }
        has_s = has_s || request.lock_mode_ == LockMode::SHARED || request.lock_mode_ == LockMode::S_IX;
        has_x = has_x || request.lock_mode_ == LockMode::EXLUCSIVE;
        has_is = has_is || request.lock_mode_ == LockMode::INTENTION_SHARED;
        has_ix = has_ix || request.lock_mode_ == LockMode::INTENTION_EXCLUSIVE || request.lock_mode_ == LockMode::S_IX;
    }
    if (has_x) {
        queue.group_lock_mode_ = GroupLockMode::X;
    } else if (has_s && has_ix) {
        queue.group_lock_mode_ = GroupLockMode::SIX;
    } else if (has_s) {
        queue.group_lock_mode_ = GroupLockMode::S;
    } else if (has_ix) {
        queue.group_lock_mode_ = GroupLockMode::IX;
    } else if (has_is) {
        queue.group_lock_mode_ = GroupLockMode::IS;
    } else {
        queue.group_lock_mode_ = GroupLockMode::NON_LOCK;
    }
}

bool LockManager::lock(Transaction *txn, LockDataId lock_data_id, LockMode lock_mode) {
    if (txn == nullptr) {
        return true;
    }
    if (txn->get_state() == TransactionState::SHRINKING || txn->get_state() == TransactionState::ABORTED) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }

    std::unique_lock<std::mutex> lock_guard(latch_);
    auto &queue = lock_table_[lock_data_id];
    auto existing = queue.request_queue_.end();
    for (auto it = queue.request_queue_.begin(); it != queue.request_queue_.end(); ++it) {
        if (it->txn_id_ == txn->get_transaction_id() && it->granted_) {
            existing = it;
            break;
        }
    }

    if (existing != queue.request_queue_.end()) {
        bool requested_is_write = lock_mode == LockMode::EXLUCSIVE || lock_mode == LockMode::INTENTION_EXCLUSIVE ||
                                  lock_mode == LockMode::S_IX;
        if (existing->lock_mode_ == lock_mode || existing->lock_mode_ == LockMode::EXLUCSIVE ||
            (!requested_is_write && existing->lock_mode_ == LockMode::SHARED)) {
            return true;
        }
        for (auto &request : queue.request_queue_) {
            if (!request.granted_ || request.txn_id_ == txn->get_transaction_id()) {
                continue;
            }
            if (!compatible(lock_mode, request.lock_mode_)) {
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
        }
        existing->lock_mode_ = lock_mode;
        refresh_group_lock_mode(queue);
        return true;
    }

    for (auto &request : queue.request_queue_) {
        if (!request.granted_ || request.txn_id_ == txn->get_transaction_id()) {
            continue;
        }
        if (!compatible(lock_mode, request.lock_mode_)) {
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }
    }

    queue.request_queue_.emplace_back(txn->get_transaction_id(), lock_mode);
    queue.request_queue_.back().granted_ = true;
    txn->get_lock_set()->insert(lock_data_id);
    refresh_group_lock_mode(queue);
    return true;
}

bool LockManager::lock_shared_on_record(Transaction *txn, const Rid &rid, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::SHARED);
}

bool LockManager::lock_exclusive_on_record(Transaction *txn, const Rid &rid, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::EXLUCSIVE);
}

bool LockManager::lock_shared_on_table(Transaction *txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::SHARED);
}

bool LockManager::lock_exclusive_on_table(Transaction *txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::EXLUCSIVE);
}

bool LockManager::lock_IS_on_table(Transaction *txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::INTENTION_SHARED);
}

bool LockManager::lock_IX_on_table(Transaction *txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::INTENTION_EXCLUSIVE);
}

bool LockManager::unlock(Transaction *txn, LockDataId lock_data_id) {
    if (txn == nullptr) {
        return true;
    }
    std::unique_lock<std::mutex> lock_guard(latch_);
    auto table_it = lock_table_.find(lock_data_id);
    if (table_it == lock_table_.end()) {
        return false;
    }
    auto &queue = table_it->second;
    for (auto it = queue.request_queue_.begin(); it != queue.request_queue_.end(); ++it) {
        if (it->txn_id_ == txn->get_transaction_id()) {
            queue.request_queue_.erase(it);
            txn->get_lock_set()->erase(lock_data_id);
            refresh_group_lock_mode(queue);
            if (queue.request_queue_.empty()) {
                lock_table_.erase(table_it);
            }
            if (txn->get_state() == TransactionState::GROWING) {
                txn->set_state(TransactionState::SHRINKING);
            }
            return true;
        }
    }
    return false;
}
