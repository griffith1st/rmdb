/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <condition_variable>
#include <list>
#include <mutex>
#include <unordered_map>

#include "transaction/transaction.h"

static const std::string GroupLockModeStr[10] = {"NON_LOCK", "IS", "IX", "S", "X", "SIX"};

class LockManager {
    enum class LockMode { SHARED, EXLUCSIVE, INTENTION_SHARED, INTENTION_EXCLUSIVE, S_IX };

    enum class GroupLockMode { NON_LOCK, IS, IX, S, X, SIX };

    class LockRequest {
       public:
        LockRequest(txn_id_t txn_id, LockMode lock_mode)
            : txn_id_(txn_id), lock_mode_(lock_mode), granted_(false) {}

        txn_id_t txn_id_;
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

    bool lock_shared_on_record(Transaction *txn, const Rid &rid, int tab_fd);

    bool lock_exclusive_on_record(Transaction *txn, const Rid &rid, int tab_fd);

    bool lock_shared_on_table(Transaction *txn, int tab_fd);

    bool lock_exclusive_on_table(Transaction *txn, int tab_fd);

    bool lock_IS_on_table(Transaction *txn, int tab_fd);

    bool lock_IX_on_table(Transaction *txn, int tab_fd);

    bool unlock(Transaction *txn, LockDataId lock_data_id);

   private:
    bool lock(Transaction *txn, LockDataId lock_data_id, LockMode lock_mode);
    bool compatible(LockMode requested, LockMode granted) const;
    void refresh_group_lock_mode(LockRequestQueue &queue);

    std::mutex latch_;
    std::unordered_map<LockDataId, LockRequestQueue> lock_table_;
};
