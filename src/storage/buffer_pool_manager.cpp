/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "buffer_pool_manager.h"

bool BufferPoolManager::find_victim_page(frame_id_t *frame_id) {
    if (!free_list_.empty()) {
        *frame_id = free_list_.front();
        free_list_.pop_front();
        return true;
    }
    return replacer_->victim(frame_id);
}

void BufferPoolManager::update_page(Page *page, PageId new_page_id, frame_id_t new_frame_id) {
    PageId old_page_id = page->id_;
    if (old_page_id.page_no != INVALID_PAGE_ID) {
        if (page->is_dirty_) {
            disk_manager_->write_page(old_page_id.fd, old_page_id.page_no, page->data_, PAGE_SIZE);
            page->is_dirty_ = false;
        }
        page_table_.erase(old_page_id);
    }

    page->reset_memory();
    page->id_ = new_page_id;
    page->pin_count_ = 0;
    page->is_dirty_ = false;
    if (new_page_id.page_no != INVALID_PAGE_ID) {
        page_table_[new_page_id] = new_frame_id;
    }
}

Page *BufferPoolManager::fetch_page(PageId page_id) {
    std::scoped_lock lock{latch_};
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        Page *page = &pages_[it->second];
        page->pin_count_++;
        replacer_->pin(it->second);
        return page;
    }

    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) {
        return nullptr;
    }

    Page *page = &pages_[frame_id];
    update_page(page, page_id, frame_id);
    disk_manager_->read_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
    page->pin_count_ = 1;
    replacer_->pin(frame_id);
    return page;
}

bool BufferPoolManager::unpin_page(PageId page_id, bool is_dirty) {
    std::scoped_lock lock{latch_};
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }
    Page *page = &pages_[it->second];
    if (page->pin_count_ <= 0) {
        return false;
    }
    page->is_dirty_ = page->is_dirty_ || is_dirty;
    page->pin_count_--;
    if (page->pin_count_ == 0) {
        replacer_->unpin(it->second);
    }
    return true;
}

bool BufferPoolManager::flush_page(PageId page_id) {
    std::scoped_lock lock{latch_};
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }
    Page *page = &pages_[it->second];
    disk_manager_->write_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
    page->is_dirty_ = false;
    return true;
}

Page *BufferPoolManager::new_page(PageId *page_id) {
    std::scoped_lock lock{latch_};
    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) {
        return nullptr;
    }
    page_id->page_no = disk_manager_->allocate_page(page_id->fd);

    Page *page = &pages_[frame_id];
    update_page(page, *page_id, frame_id);
    page->pin_count_ = 1;
    page->is_dirty_ = false;
    replacer_->pin(frame_id);
    return page;
}

bool BufferPoolManager::delete_page(PageId page_id) {
    std::scoped_lock lock{latch_};
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        disk_manager_->deallocate_page(page_id.page_no);
        return true;
    }

    frame_id_t frame_id = it->second;
    Page *page = &pages_[frame_id];
    if (page->pin_count_ > 0) {
        return false;
    }

    replacer_->pin(frame_id);
    page_table_.erase(it);
    page->reset_memory();
    page->id_ = PageId{page_id.fd, INVALID_PAGE_ID};
    page->pin_count_ = 0;
    page->is_dirty_ = false;
    free_list_.push_back(frame_id);
    disk_manager_->deallocate_page(page_id.page_no);
    return true;
}

void BufferPoolManager::flush_all_pages(int fd) {
    std::scoped_lock lock{latch_};
    for (size_t i = 0; i < pool_size_; ++i) {
        Page *page = &pages_[i];
        if (page->id_.fd == fd && page->id_.page_no != INVALID_PAGE_ID) {
            disk_manager_->write_page(fd, page->id_.page_no, page->data_, PAGE_SIZE);
            page->is_dirty_ = false;
        }
    }
}
