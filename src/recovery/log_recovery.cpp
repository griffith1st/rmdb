/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "log_recovery.h"

#include <cstring>

#include "execution/index_utils.h"
#include "record/rm_file_handle.h"

namespace {
std::string table_name_from(const char *name, size_t size) {
    return std::string(name, size);
}

bool record_exists(RmFileHandle *fh, const Rid &rid) {
    try {
        auto rec = fh->get_record(rid, nullptr);
        (void)rec;
        return true;
    } catch (RMDBError &) {
        return false;
    }
}

void insert_indexes(SmManager *sm_manager, const std::string &tab_name, const TabMeta &tab,
                    const RmRecord &rec, const Rid &rid) {
    for (auto &index : tab.indexes) {
        auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
        auto key = make_index_key(index, rec.data);
        try {
            ih->insert_entry(key.get(), rid, nullptr);
        } catch (RMDBError &) {
        }
    }
}

void delete_indexes(SmManager *sm_manager, const std::string &tab_name, const TabMeta &tab,
                    const RmRecord &rec) {
    for (auto &index : tab.indexes) {
        auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
        auto key = make_index_key(index, rec.data);
        ih->delete_entry(key.get(), nullptr);
    }
}

void redo_insert(SmManager *sm_manager, const InsertLogRecord *log) {
    std::string tab_name = table_name_from(log->table_name_, log->table_name_size_);
    TabMeta &tab = sm_manager->db_.get_table(tab_name);
    RmFileHandle *fh = sm_manager->fhs_.at(tab_name).get();
    if (record_exists(fh, log->rid_)) {
        auto old = fh->get_record(log->rid_, nullptr);
        delete_indexes(sm_manager, tab_name, tab, *old);
        fh->update_record(log->rid_, log->insert_value_.data, nullptr);
    } else {
        fh->insert_record(log->rid_, log->insert_value_.data);
    }
    insert_indexes(sm_manager, tab_name, tab, log->insert_value_, log->rid_);
}

void redo_delete(SmManager *sm_manager, const DeleteLogRecord *log) {
    std::string tab_name = table_name_from(log->table_name_, log->table_name_size_);
    TabMeta &tab = sm_manager->db_.get_table(tab_name);
    RmFileHandle *fh = sm_manager->fhs_.at(tab_name).get();
    if (!record_exists(fh, log->rid_)) {
        return;
    }
    auto old = fh->get_record(log->rid_, nullptr);
    delete_indexes(sm_manager, tab_name, tab, *old);
    fh->delete_record(log->rid_, nullptr);
}

void redo_update(SmManager *sm_manager, const UpdateLogRecord *log) {
    std::string tab_name = table_name_from(log->table_name_, log->table_name_size_);
    TabMeta &tab = sm_manager->db_.get_table(tab_name);
    RmFileHandle *fh = sm_manager->fhs_.at(tab_name).get();
    if (!record_exists(fh, log->rid_)) {
        return;
    }
    auto old = fh->get_record(log->rid_, nullptr);
    delete_indexes(sm_manager, tab_name, tab, *old);
    fh->update_record(log->rid_, log->new_value_.data, nullptr);
    insert_indexes(sm_manager, tab_name, tab, log->new_value_, log->rid_);
}

void undo_insert(SmManager *sm_manager, const InsertLogRecord *log) {
    std::string tab_name = table_name_from(log->table_name_, log->table_name_size_);
    TabMeta &tab = sm_manager->db_.get_table(tab_name);
    RmFileHandle *fh = sm_manager->fhs_.at(tab_name).get();
    if (!record_exists(fh, log->rid_)) {
        return;
    }
    auto old = fh->get_record(log->rid_, nullptr);
    delete_indexes(sm_manager, tab_name, tab, *old);
    fh->delete_record(log->rid_, nullptr);
}

void undo_delete(SmManager *sm_manager, const DeleteLogRecord *log) {
    std::string tab_name = table_name_from(log->table_name_, log->table_name_size_);
    TabMeta &tab = sm_manager->db_.get_table(tab_name);
    RmFileHandle *fh = sm_manager->fhs_.at(tab_name).get();
    if (record_exists(fh, log->rid_)) {
        auto old = fh->get_record(log->rid_, nullptr);
        delete_indexes(sm_manager, tab_name, tab, *old);
        fh->update_record(log->rid_, log->delete_value_.data, nullptr);
    } else {
        fh->insert_record(log->rid_, log->delete_value_.data);
    }
    insert_indexes(sm_manager, tab_name, tab, log->delete_value_, log->rid_);
}

void undo_update(SmManager *sm_manager, const UpdateLogRecord *log) {
    std::string tab_name = table_name_from(log->table_name_, log->table_name_size_);
    TabMeta &tab = sm_manager->db_.get_table(tab_name);
    RmFileHandle *fh = sm_manager->fhs_.at(tab_name).get();
    if (!record_exists(fh, log->rid_)) {
        return;
    }
    auto old = fh->get_record(log->rid_, nullptr);
    delete_indexes(sm_manager, tab_name, tab, *old);
    fh->update_record(log->rid_, log->old_value_.data, nullptr);
    insert_indexes(sm_manager, tab_name, tab, log->old_value_, log->rid_);
}
}

void RecoveryManager::analyze() {
    logs_.clear();
    committed_txns_.clear();
    aborted_txns_.clear();
    active_txns_.clear();

    int file_size = disk_manager_->get_file_size(LOG_FILE_NAME);
    if (file_size <= 0) {
        return;
    }
    std::vector<char> data(file_size);
    int read_size = disk_manager_->read_log(data.data(), file_size, 0);
    int offset = 0;
    while (offset + LOG_HEADER_SIZE <= read_size) {
        LogType type = *reinterpret_cast<LogType *>(data.data() + offset + OFFSET_LOG_TYPE);
        uint32_t len = *reinterpret_cast<uint32_t *>(data.data() + offset + OFFSET_LOG_TOT_LEN);
        if (len < LOG_HEADER_SIZE || offset + static_cast<int>(len) > read_size) {
            break;
        }

        std::unique_ptr<LogRecord> log;
        if (type == LogType::begin) {
            log = std::make_unique<BeginLogRecord>();
        } else if (type == LogType::commit) {
            log = std::make_unique<CommitLogRecord>();
        } else if (type == LogType::ABORT) {
            log = std::make_unique<AbortLogRecord>();
        } else if (type == LogType::INSERT) {
            log = std::make_unique<InsertLogRecord>();
        } else if (type == LogType::DELETE) {
            log = std::make_unique<DeleteLogRecord>();
        } else if (type == LogType::UPDATE) {
            log = std::make_unique<UpdateLogRecord>();
        } else {
            break;
        }
        log->deserialize(data.data() + offset);
        if (log->log_type_ == LogType::begin) {
            active_txns_.insert(log->log_tid_);
        } else if (log->log_type_ == LogType::commit) {
            committed_txns_.insert(log->log_tid_);
            active_txns_.erase(log->log_tid_);
        } else if (log->log_type_ == LogType::ABORT) {
            aborted_txns_.insert(log->log_tid_);
            active_txns_.erase(log->log_tid_);
        }
        logs_.push_back(std::move(log));
        offset += len;
    }
}

void RecoveryManager::redo() {
    for (auto &log : logs_) {
        if (!committed_txns_.count(log->log_tid_)) {
            continue;
        }
        if (log->log_type_ == LogType::INSERT) {
            redo_insert(sm_manager_, static_cast<InsertLogRecord *>(log.get()));
        } else if (log->log_type_ == LogType::DELETE) {
            redo_delete(sm_manager_, static_cast<DeleteLogRecord *>(log.get()));
        } else if (log->log_type_ == LogType::UPDATE) {
            redo_update(sm_manager_, static_cast<UpdateLogRecord *>(log.get()));
        }
    }
}

void RecoveryManager::undo() {
    for (auto it = logs_.rbegin(); it != logs_.rend(); ++it) {
        auto &log = *it;
        if (!active_txns_.count(log->log_tid_)) {
            continue;
        }
        if (log->log_type_ == LogType::INSERT) {
            undo_insert(sm_manager_, static_cast<InsertLogRecord *>(log.get()));
        } else if (log->log_type_ == LogType::DELETE) {
            undo_delete(sm_manager_, static_cast<DeleteLogRecord *>(log.get()));
        } else if (log->log_type_ == LogType::UPDATE) {
            undo_update(sm_manager_, static_cast<UpdateLogRecord *>(log.get()));
        }
    }
    for (auto &entry : sm_manager_->fhs_) {
        buffer_pool_manager_->flush_all_pages(entry.second->GetFd());
    }
}
