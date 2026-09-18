/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <memory>
#include <set>
#include <vector>

#include "log_manager.h"
#include "storage/disk_manager.h"
#include "system/sm_manager.h"

class RecoveryManager {
   public:
    RecoveryManager(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, SmManager *sm_manager) {
        disk_manager_ = disk_manager;
        buffer_pool_manager_ = buffer_pool_manager;
        sm_manager_ = sm_manager;
    }

    void analyze();
    void redo();
    void undo();

    lsn_t next_lsn() const { return next_lsn_; }
    txn_id_t next_txn_id() const { return next_txn_id_; }

   private:
    std::vector<std::unique_ptr<LogRecord>> logs_;
    std::set<txn_id_t> committed_txns_;
    std::set<txn_id_t> aborted_txns_;
    std::set<txn_id_t> active_txns_;
    lsn_t next_lsn_ = 0;
    txn_id_t next_txn_id_ = 0;

    DiskManager *disk_manager_;
    BufferPoolManager *buffer_pool_manager_;
    SmManager *sm_manager_;
};
