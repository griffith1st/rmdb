/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "transaction_manager.h"

#include <vector>

#include "execution/index_utils.h"
#include "index/ix.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

namespace {
void insert_indexes(SmManager *sm_manager, const std::string &tab_name, const TabMeta &tab,
                    const RmRecord &rec, const Rid &rid, Transaction *txn) {
    for (auto &index : tab.indexes) {
        auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
        auto key = make_index_key(index, rec.data);
        ih->insert_entry(key.get(), rid, txn);
    }
}

void delete_indexes(SmManager *sm_manager, const std::string &tab_name, const TabMeta &tab,
                    const RmRecord &rec, Transaction *txn) {
    for (auto &index : tab.indexes) {
        auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
        auto key = make_index_key(index, rec.data);
        ih->delete_entry(key.get(), txn);
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
    if (txn == nullptr) {
        return;
    }
    auto write_set = txn->get_write_set();
    while (write_set != nullptr && !write_set->empty()) {
        delete write_set->back();
        write_set->pop_back();
    }
    if (log_manager != nullptr) {
        CommitLogRecord log_record(txn->get_transaction_id());
        log_record.prev_lsn_ = txn->get_prev_lsn();
        lsn_t lsn = log_manager->add_log_to_buffer(&log_record);
        txn->set_prev_lsn(lsn);
    }
    txn->set_state(TransactionState::COMMITTED);
    if (lock_manager_ != nullptr) {
        std::vector<LockDataId> locks(txn->get_lock_set()->begin(), txn->get_lock_set()->end());
        for (auto &lock_data_id : locks) {
            lock_manager_->unlock(txn, lock_data_id);
        }
    }
    if (log_manager != nullptr) {
        log_manager->flush_log_to_disk();
    }
    std::unique_lock<std::mutex> lock(latch_);
    txn_map.erase(txn->get_transaction_id());
}

void TransactionManager::abort(Transaction *txn, LogManager *log_manager) {
    if (txn == nullptr) {
        return;
    }
    auto write_set = txn->get_write_set();
    while (write_set != nullptr && !write_set->empty()) {
        WriteRecord *write_record = write_set->back();
        write_set->pop_back();
        const std::string tab_name = write_record->GetTableName();
        TabMeta &tab = sm_manager_->db_.get_table(tab_name);
        RmFileHandle *fh = sm_manager_->fhs_.at(tab_name).get();
        Rid rid = write_record->GetRid();

        switch (write_record->GetWriteType()) {
            case WType::INSERT_TUPLE: {
                try {
                    auto rec = fh->get_record(rid, nullptr);
                    delete_indexes(sm_manager_, tab_name, tab, *rec, txn);
                    fh->delete_record(rid, nullptr);
                } catch (RecordNotFoundError &) {
                }
                break;
            }
            case WType::DELETE_TUPLE: {
                RmRecord &old_rec = write_record->GetRecord();
                fh->insert_record(rid, old_rec.data);
                insert_indexes(sm_manager_, tab_name, tab, old_rec, rid, txn);
                break;
            }
            case WType::UPDATE_TUPLE: {
                RmRecord &old_rec = write_record->GetRecord();
                auto cur_rec = fh->get_record(rid, nullptr);
                delete_indexes(sm_manager_, tab_name, tab, *cur_rec, txn);
                fh->update_record(rid, old_rec.data, nullptr);
                insert_indexes(sm_manager_, tab_name, tab, old_rec, rid, txn);
                break;
            }
        }
        delete write_record;
    }
    if (log_manager != nullptr) {
        AbortLogRecord log_record(txn->get_transaction_id());
        log_record.prev_lsn_ = txn->get_prev_lsn();
        lsn_t lsn = log_manager->add_log_to_buffer(&log_record);
        txn->set_prev_lsn(lsn);
    }
    txn->set_state(TransactionState::ABORTED);
    if (lock_manager_ != nullptr) {
        std::vector<LockDataId> locks(txn->get_lock_set()->begin(), txn->get_lock_set()->end());
        for (auto &lock_data_id : locks) {
            lock_manager_->unlock(txn, lock_data_id);
        }
    }
    if (log_manager != nullptr) {
        log_manager->flush_log_to_disk();
    }
    std::unique_lock<std::mutex> lock(latch_);
    txn_map.erase(txn->get_transaction_id());
}
