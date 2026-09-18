/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "rm_file_handle.h"

#include <cstring>

std::unique_ptr<RmRecord> RmFileHandle::get_record(const Rid &rid, Context *context) const {
    (void)context;
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page ||
        !Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }
    auto record = std::make_unique<RmRecord>(file_hdr_.record_size, page_handle.get_slot(rid.slot_no));
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
    return record;
}

Rid RmFileHandle::insert_record(char *buf, Context *context,
                              const std::function<void(const Rid &)> &before_insert) {
    (void)context;
    RmPageHandle page_handle = create_page_handle();
    int slot_no = Bitmap::first_bit(false, page_handle.bitmap, file_hdr_.num_records_per_page);
    if (slot_no >= file_hdr_.num_records_per_page) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw InternalError("No free slot in record page");
    }

    const Rid rid{page_handle.page->get_page_id().page_no, slot_no};
    try {
        if (before_insert) before_insert(rid);
    } catch (...) {
        // A newly allocated page still owns initialized free-list metadata.
        // Keep it dirty, but leave its bitmap and record count unchanged.
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
        throw;
    }

    memcpy(page_handle.get_slot(slot_no), buf, file_hdr_.record_size);
    Bitmap::set(page_handle.bitmap, slot_no);
    page_handle.page_hdr->num_records++;

    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
        page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
    }

    disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, reinterpret_cast<char *>(&file_hdr_), sizeof(file_hdr_));
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
    return rid;
}

void RmFileHandle::insert_record(const Rid &rid, char *buf) {
    if (rid.page_no < RM_FIRST_RECORD_PAGE || rid.slot_no < 0 ||
        rid.slot_no >= file_hdr_.num_records_per_page) {
        throw InternalError("Invalid insert rid");
    }
    // Redo can reference pages whose allocation never reached disk before a crash.
    while (rid.page_no >= file_hdr_.num_pages) {
        auto new_page = create_new_page_handle();
        buffer_pool_manager_->unpin_page(new_page.page->get_page_id(), true);
    }
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    if (Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw InternalError("Invalid insert rid");
    }
    memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);
    Bitmap::set(page_handle.bitmap, rid.slot_no);
    page_handle.page_hdr->num_records++;
    const bool became_full = page_handle.page_hdr->num_records == file_hdr_.num_records_per_page;
    const int next_free = page_handle.page_hdr->next_free_page_no;
    if (became_full) {
        page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
    }
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
    if (became_full) {
        if (file_hdr_.first_free_page_no == rid.page_no) {
            file_hdr_.first_free_page_no = next_free;
        } else {
            int predecessor = file_hdr_.first_free_page_no;
            while (predecessor != RM_NO_PAGE) {
                auto previous = fetch_page_handle(predecessor);
                const int next = previous.page_hdr->next_free_page_no;
                const bool found = next == rid.page_no;
                if (found) previous.page_hdr->next_free_page_no = next_free;
                buffer_pool_manager_->unpin_page(previous.page->get_page_id(), found);
                if (found) break;
                predecessor = next;
            }
        }
    }
    disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, reinterpret_cast<char *>(&file_hdr_), sizeof(file_hdr_));
}

void RmFileHandle::delete_record(const Rid &rid, Context *context) {
    (void)context;
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page ||
        !Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    bool was_full = page_handle.page_hdr->num_records == file_hdr_.num_records_per_page;
    Bitmap::reset(page_handle.bitmap, rid.slot_no);
    page_handle.page_hdr->num_records--;
    if (was_full) {
        release_page_handle(page_handle);
    }

    disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, reinterpret_cast<char *>(&file_hdr_), sizeof(file_hdr_));
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}

void RmFileHandle::update_record(const Rid &rid, char *buf, Context *context) {
    (void)context;
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page ||
        !Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }
    memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}

void RmFileHandle::rebuild_free_page_list() {
    int first_free = RM_NO_PAGE;
    for (int page_no = RM_FIRST_RECORD_PAGE; page_no < file_hdr_.num_pages; ++page_no) {
        auto page = fetch_page_handle(page_no);
        int count = 0;
        for (int slot = 0; slot < file_hdr_.num_records_per_page; ++slot) {
            if (Bitmap::is_set(page.bitmap, slot)) ++count;
        }
        const int next_free = count < file_hdr_.num_records_per_page ? first_free : RM_NO_PAGE;
        const bool changed = page.page_hdr->num_records != count ||
                             page.page_hdr->next_free_page_no != next_free;
        page.page_hdr->num_records = count;
        page.page_hdr->next_free_page_no = next_free;
        if (count < file_hdr_.num_records_per_page) first_free = page_no;
        buffer_pool_manager_->unpin_page(page.page->get_page_id(), changed);
    }
    file_hdr_.first_free_page_no = first_free;
    disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, reinterpret_cast<char *>(&file_hdr_), sizeof(file_hdr_));
}

RmPageHandle RmFileHandle::fetch_page_handle(int page_no) const {
    if (page_no < RM_FIRST_RECORD_PAGE || page_no >= file_hdr_.num_pages) {
        throw PageNotExistError(disk_manager_->get_file_name(fd_), page_no);
    }
    PageId page_id{fd_, page_no};
    Page *page = buffer_pool_manager_->fetch_page(page_id);
    if (page == nullptr) {
        throw InternalError("Fetch record page failed");
    }
    return RmPageHandle(&file_hdr_, page);
}

RmPageHandle RmFileHandle::create_new_page_handle() {
    PageId page_id{fd_, INVALID_PAGE_ID};
    Page *page = buffer_pool_manager_->new_page(&page_id);
    if (page == nullptr) {
        throw InternalError("Create record page failed");
    }

    file_hdr_.num_pages++;
    RmPageHandle page_handle(&file_hdr_, page);
    page_handle.page_hdr->num_records = 0;
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    Bitmap::init(page_handle.bitmap, file_hdr_.bitmap_size);
    file_hdr_.first_free_page_no = page_id.page_no;

    // Publish a valid empty page before the header starts referring to it.
    // Otherwise a process crash can leave an all-zero page whose next pointer
    // incorrectly refers to the file header (page 0).
    disk_manager_->write_page(fd_, page_id.page_no, page->get_data(), PAGE_SIZE);
    disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, reinterpret_cast<char *>(&file_hdr_), sizeof(file_hdr_));
    return page_handle;
}

RmPageHandle RmFileHandle::create_page_handle() {
    if (file_hdr_.first_free_page_no == RM_NO_PAGE) {
        return create_new_page_handle();
    }
    return fetch_page_handle(file_hdr_.first_free_page_no);
}

void RmFileHandle::release_page_handle(RmPageHandle &page_handle) {
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    file_hdr_.first_free_page_no = page_handle.page->get_page_id().page_no;
}
