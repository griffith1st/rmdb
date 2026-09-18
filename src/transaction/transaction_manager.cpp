/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "transaction_manager.h"

#include <vector>
#include <set>

#include "execution/index_utils.h"
#include "index/ix.h"
#include "record/rm_file_handle.h"
#include "record/rm_scan.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

namespace {
void rebuild_indexes(SmManager *sm_manager, const std::string &tab_name) {
    auto *fh = sm_manager->fhs_.at(tab_name).get();
    auto *ix = sm_manager->get_ix_manager();
    for (auto &index : sm_manager->db_.get_table(tab_name).indexes) {
        auto &ih = sm_manager->ihs_.at(ix->get_index_name(tab_name, index.cols));
        ih.reset();
        ih = ix->open_index(tab_name, index.cols);
        for (RmScan scan(fh); !scan.is_end(); scan.next()) {
            Rid rid = scan.rid();
            auto record = fh->get_record(rid, nullptr);
            auto key = make_index_key(index, record->data);
            ih->insert_entry(key.get(), rid, nullptr);
        }
    }
}
}

Transaction *TransactionManager::begin(Transaction *txn, LogManager *log_manager) {
    if (txn == nullptr) {
        txn = new Transaction(next_txn_id_++);
        txn->set_start_ts(next_timestamp_++);
    }
    txn->set_state(TransactionState::GROWING);
    if (log_manager != nullptr) {
        BeginLogRecord log_record(txn->get_transaction_id());
        log_record.prev_lsn_ = txn->get_prev_lsn();
        lsn_t lsn = log_manager->add_log_to_buffer(&log_record);
        txn->set_prev_lsn(lsn);
    }
    std::unique_lock<std::mutex> lock(latch_);
    txn_map[txn->get_transaction_id()] = txn;
    return txn;
}

void TransactionManager::commit(Transaction *txn, LogManager *log_manager) {
    if (txn == nullptr || txn->get_state() == TransactionState::COMMITTED ||
        txn->get_state() == TransactionState::ABORTED) {
        return;
    }
    if (log_manager != nullptr) {
        CommitLogRecord log_record(txn->get_transaction_id());
        log_record.prev_lsn_ = txn->get_prev_lsn();
        lsn_t lsn = log_manager->add_log_to_buffer(&log_record);
        txn->set_prev_lsn(lsn);
        // A successful commit must be durable before another transaction can
        // observe its values through released locks.
        log_manager->flush_log_to_disk();
    }
    auto write_set = txn->get_write_set();
    while (write_set != nullptr && !write_set->empty()) {
        delete write_set->back();
        write_set->pop_back();
    }
    txn->set_state(TransactionState::COMMITTED);
    if (lock_manager_ != nullptr) {
        std::vector<LockDataId> locks(txn->get_lock_set()->begin(), txn->get_lock_set()->end());
        for (auto &lock_data_id : locks) {
            lock_manager_->unlock(txn, lock_data_id);
        }
    }
    std::unique_lock<std::mutex> lock(latch_);
    txn_map.erase(txn->get_transaction_id());
}

void TransactionManager::abort(Transaction *txn, LogManager *log_manager) {
    if (txn == nullptr || txn->get_state() == TransactionState::COMMITTED ||
        txn->get_state() == TransactionState::ABORTED) {
        return;
    }
    auto write_set = txn->get_write_set();
    std::set<std::string> changed_tables;
    while (write_set != nullptr && !write_set->empty()) {
        WriteRecord *write_record = write_set->back();
        write_set->pop_back();
        const std::string tab_name = write_record->GetTableName();
        changed_tables.insert(tab_name);
        RmFileHandle *fh = sm_manager_->fhs_.at(tab_name).get();
        Rid rid = write_record->GetRid();

        switch (write_record->GetWriteType()) {
            case WType::INSERT_TUPLE: {
                try {
                    fh->delete_record(rid, nullptr);
                } catch (RecordNotFoundError &) {
                }
                break;
            }
            case WType::DELETE_TUPLE: {
                RmRecord &old_rec = write_record->GetRecord();
                fh->insert_record(rid, old_rec.data);
                break;
            }
            case WType::UPDATE_TUPLE: {
                RmRecord &old_rec = write_record->GetRecord();
                fh->update_record(rid, old_rec.data, nullptr);
                break;
            }
        }
        delete write_record;
    }
    // Restore the whole heap before rebuilding unique keys. Per-row index undo
    // conflicts with other rows from an atomic key shift (1,2 -> 2,3).
    for (const auto &table : changed_tables) rebuild_indexes(sm_manager_, table);
    if (log_manager != nullptr) {
        AbortLogRecord log_record(txn->get_transaction_id());
        log_record.prev_lsn_ = txn->get_prev_lsn();
        lsn_t lsn = log_manager->add_log_to_buffer(&log_record);
        txn->set_prev_lsn(lsn);
        log_manager->flush_log_to_disk();
    }
    txn->set_state(TransactionState::ABORTED);
    if (lock_manager_ != nullptr) {
        std::vector<LockDataId> locks(txn->get_lock_set()->begin(), txn->get_lock_set()->end());
        for (auto &lock_data_id : locks) {
            lock_manager_->unlock(txn, lock_data_id);
        }
    }
    std::unique_lock<std::mutex> lock(latch_);
    txn_map.erase(txn->get_transaction_id());
}
