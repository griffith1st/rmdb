/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "log_recovery.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <tuple>
#include <unistd.h>

#include "execution/index_utils.h"
#include "record/rm_scan.h"

namespace {
// Validate lengths before deserializers allocate or copy a possibly torn record.
bool valid_payload(const char *data, size_t length, LogType type) {
    if (type == LogType::begin || type == LogType::commit || type == LogType::ABORT) {
        return length == LOG_HEADER_SIZE;
    }
    size_t offset = LOG_HEADER_SIZE;
    int images = type == LogType::UPDATE ? 2 : 1;
    for (int i = 0; i < images; ++i) {
        if (length - offset < sizeof(int)) return false;
        int size;
        memcpy(&size, data + offset, sizeof(size));
        offset += sizeof(size);
        if (size <= 0 || size > RM_MAX_RECORD_SIZE || static_cast<size_t>(size) > length - offset) return false;
        offset += size;
    }
    if (length - offset < sizeof(Rid) + sizeof(size_t)) return false;
    offset += sizeof(Rid);
    size_t name_size;
    memcpy(&name_size, data + offset, sizeof(name_size));
    offset += sizeof(name_size);
    return name_size > 0 && name_size == length - offset;
}

struct Change {
    std::string table;
    Rid rid;
    const RmRecord *before;
    const RmRecord *after;
};

bool change_from(const LogRecord *record, Change &change) {
    if (record->log_type_ == LogType::INSERT) {
        auto log = static_cast<const InsertLogRecord *>(record);
        change = {std::string(log->table_name_, log->table_name_size_), log->rid_, nullptr, &log->insert_value_};
    } else if (record->log_type_ == LogType::DELETE) {
        auto log = static_cast<const DeleteLogRecord *>(record);
        change = {std::string(log->table_name_, log->table_name_size_), log->rid_, &log->delete_value_, nullptr};
    } else if (record->log_type_ == LogType::UPDATE) {
        auto log = static_cast<const UpdateLogRecord *>(record);
        change = {std::string(log->table_name_, log->table_name_size_), log->rid_, &log->old_value_, &log->new_value_};
    } else {
        return false;
    }
    return true;
}
}

void RecoveryManager::analyze() {
    logs_.clear();
    committed_txns_.clear();
    aborted_txns_.clear();
    active_txns_.clear();
    next_lsn_ = 0;
    next_txn_id_ = 0;
    // CREATE metadata may survive even when its buffered BEGIN did not. Do not
    // issue new DML LSNs below a persisted table-generation boundary.
    for (const auto &entry : sm_manager_->fhs_) {
        next_lsn_ = std::max(next_lsn_, sm_manager_->table_log_start(entry.first));
    }

    int file_size = disk_manager_->get_file_size(LOG_FILE_NAME);
    if (file_size <= 0) return;
    std::vector<char> data(file_size);
    int read_size = disk_manager_->read_log(data.data(), file_size, 0);
    size_t offset = 0;
    while (offset + LOG_HEADER_SIZE <= static_cast<size_t>(read_size)) {
        LogType type;
        uint32_t len;
        memcpy(&type, data.data() + offset + OFFSET_LOG_TYPE, sizeof(type));
        memcpy(&len, data.data() + offset + OFFSET_LOG_TOT_LEN, sizeof(len));
        if (len < LOG_HEADER_SIZE || len > static_cast<size_t>(read_size) - offset) break;
        if (type < LogType::UPDATE || type > LogType::ABORT ||
            !valid_payload(data.data() + offset, len, type)) {
            throw InternalError("Invalid WAL record payload");
        }

        std::unique_ptr<LogRecord> log;
        switch (type) {
            case LogType::begin: log = std::make_unique<BeginLogRecord>(); break;
            case LogType::commit: log = std::make_unique<CommitLogRecord>(); break;
            case LogType::ABORT: log = std::make_unique<AbortLogRecord>(); break;
            case LogType::INSERT: log = std::make_unique<InsertLogRecord>(); break;
            case LogType::DELETE: log = std::make_unique<DeleteLogRecord>(); break;
            case LogType::UPDATE: log = std::make_unique<UpdateLogRecord>(); break;
        }
        log->deserialize(data.data() + offset);
        if (log->lsn_ < 0 || log->log_tid_ < 0 ||
            log->lsn_ == std::numeric_limits<lsn_t>::max() ||
            log->log_tid_ == std::numeric_limits<txn_id_t>::max()) {
            throw InternalError("Invalid or exhausted WAL identifier");
        }
        next_lsn_ = std::max(next_lsn_, log->lsn_ + 1);
        next_txn_id_ = std::max(next_txn_id_, log->log_tid_ + 1);
        if (type == LogType::begin) {
            active_txns_.insert(log->log_tid_);
        } else if (type == LogType::commit) {
            committed_txns_.insert(log->log_tid_);
            active_txns_.erase(log->log_tid_);
        } else if (type == LogType::ABORT) {
            aborted_txns_.insert(log->log_tid_);
            active_txns_.erase(log->log_tid_);
        }
        logs_.push_back(std::move(log));
        offset += len;
    }
    // Future appends must not remain hidden behind an incomplete tail forever.
    if (offset != static_cast<size_t>(read_size)) {
        int fd = disk_manager_->GetLogFd();
        if (ftruncate(fd, static_cast<off_t>(offset)) != 0 || fdatasync(fd) != 0) throw UnixError();
    }
}

void RecoveryManager::redo() {
    // Header and data pages can reach disk independently. Normalize free-space
    // metadata before replay so a repeated delete cannot link a page to itself.
    for (auto &entry : sm_manager_->fhs_) entry.second->rebuild_free_page_list();
    // The complete WAL is retained, without page LSNs or checkpoints. Derive
    // each touched slot from its earliest before image and committed changes.
    // This remains idempotent if a loser used a slot later reused by a winner.
    using Slot = std::tuple<std::string, int, int>;
    std::map<Slot, const RmRecord *> final_images;
    for (const auto &log : logs_) {
        Change change;
        if (!change_from(log.get(), change)) continue;
        if (log->lsn_ < sm_manager_->table_log_start(change.table)) continue;
        Slot slot{change.table, change.rid.page_no, change.rid.slot_no};
        final_images.emplace(slot, change.before);
        if (committed_txns_.count(log->log_tid_) && !aborted_txns_.count(log->log_tid_)) {
            final_images[slot] = change.after;
        }
    }
    for (const auto &entry : final_images) {
        const auto &[table, page, slot] = entry.first;
        // A dropped table has no surviving heap; recreated names were filtered
        // by their durable creation boundary above.
        auto handle = sm_manager_->fhs_.find(table);
        if (handle == sm_manager_->fhs_.end()) continue;
        auto *fh = handle->second.get();
        Rid rid{page, slot};
        const RmRecord *image = entry.second;
        if (image != nullptr && image->size != fh->get_file_hdr().record_size) {
            throw InternalError("WAL record does not match the current table schema");
        }
        bool exists = fh->is_record(rid);
        if (image == nullptr) {
            if (exists) fh->delete_record(rid, nullptr);
        } else if (exists) {
            fh->update_record(rid, image->data, nullptr);
        } else {
            fh->insert_record(rid, image->data);
        }
    }
}

void RecoveryManager::undo() {
    // Loser/aborted changes were excluded above. Build indexes from the final
    // heap so intermediate historical keys cannot cause duplicate conflicts.
    for (auto &entry : sm_manager_->fhs_) {
        auto *fh = entry.second.get();
        fh->rebuild_free_page_list();
        for (auto &index : sm_manager_->db_.get_table(entry.first).indexes) {
            auto *ix = sm_manager_->get_ix_manager();
            std::string name = ix->get_index_name(entry.first, index.cols);
            auto &ih = sm_manager_->ihs_[name];
            ih.reset();
            ih = ix->open_index(entry.first, index.cols);
            for (RmScan scan(fh); !scan.is_end(); scan.next()) {
                Rid rid = scan.rid();
                auto record = fh->get_record(rid, nullptr);
                auto key = make_index_key(index, record->data);
                ih->insert_entry(key.get(), rid, nullptr);
            }
        }
        buffer_pool_manager_->flush_all_pages(fh->GetFd());
    }
    logs_.clear();
}
