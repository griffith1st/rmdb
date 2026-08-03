#include "ix_index_handle.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace {
std::map<int, std::vector<std::pair<std::vector<char>, Rid>>> g_index_entries;
std::mutex g_index_latch;

int compare_key(const IxFileHdr *hdr, const char *lhs, const char *rhs) {
    return ix_compare(lhs, rhs, hdr->col_types_, hdr->col_lens_);
}

int compare_key(const IxFileHdr *hdr, const std::vector<char> &lhs, const char *rhs) {
    return compare_key(hdr, lhs.data(), rhs);
}

auto lower_entry(const IxFileHdr *hdr, std::vector<std::pair<std::vector<char>, Rid>> &entries, const char *key) {
    return std::lower_bound(entries.begin(), entries.end(), key,
                            [hdr](const auto &entry, const char *target) {
                                return compare_key(hdr, entry.first, target) < 0;
                            });
}

auto lower_entry(const IxFileHdr *hdr, const std::vector<std::pair<std::vector<char>, Rid>> &entries, const char *key) {
    return std::lower_bound(entries.begin(), entries.end(), key,
                            [hdr](const auto &entry, const char *target) {
                                return compare_key(hdr, entry.first, target) < 0;
                            });
}
}

int IxNodeHandle::lower_bound(const char *target) const {
    int size = page_hdr->num_key;
    for (int i = 0; i < size; ++i) {
        if (compare_key(file_hdr, get_key(i), target) >= 0) {
            return i;
        }
    }
    return size;
}

int IxNodeHandle::upper_bound(const char *target) const {
    int size = page_hdr->num_key;
    for (int i = 0; i < size; ++i) {
        if (compare_key(file_hdr, get_key(i), target) > 0) {
            return i;
        }
    }
    return size;
}

bool IxNodeHandle::leaf_lookup(const char *key, Rid **value) {
    int pos = lower_bound(key);
    if (pos < get_size() && compare_key(file_hdr, get_key(pos), key) == 0) {
        *value = get_rid(pos);
        return true;
    }
    return false;
}

page_id_t IxNodeHandle::internal_lookup(const char *key) {
    int pos = upper_bound(key);
    if (pos == 0) {
        return value_at(0);
    }
    return value_at(pos - 1);
}

void IxNodeHandle::insert_pairs(int pos, const char *key, const Rid *rid, int n) {
    assert(pos >= 0 && pos <= get_size());
    int move_count = get_size() - pos;
    if (move_count > 0) {
        memmove(get_key(pos + n), get_key(pos), move_count * file_hdr->col_tot_len_);
        memmove(get_rid(pos + n), get_rid(pos), move_count * sizeof(Rid));
    }
    memcpy(get_key(pos), key, n * file_hdr->col_tot_len_);
    memcpy(get_rid(pos), rid, n * sizeof(Rid));
    set_size(get_size() + n);
}

int IxNodeHandle::insert(const char *key, const Rid &value) {
    int pos = lower_bound(key);
    if (pos < get_size() && compare_key(file_hdr, get_key(pos), key) == 0) {
        return get_size();
    }
    insert_pair(pos, key, value);
    return get_size();
}

void IxNodeHandle::erase_pair(int pos) {
    assert(pos >= 0 && pos < get_size());
    int move_count = get_size() - pos - 1;
    if (move_count > 0) {
        memmove(get_key(pos), get_key(pos + 1), move_count * file_hdr->col_tot_len_);
        memmove(get_rid(pos), get_rid(pos + 1), move_count * sizeof(Rid));
    }
    set_size(get_size() - 1);
}

int IxNodeHandle::remove(const char *key) {
    int pos = lower_bound(key);
    if (pos < get_size() && compare_key(file_hdr, get_key(pos), key) == 0) {
        erase_pair(pos);
    }
    return get_size();
}

IxIndexHandle::IxIndexHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd)
    : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager), fd_(fd) {
    char *buf = new char[PAGE_SIZE];
    memset(buf, 0, PAGE_SIZE);
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, buf, PAGE_SIZE);
    file_hdr_ = new IxFileHdr();
    file_hdr_->deserialize(buf);
    delete[] buf;
    int now_page_no = disk_manager_->get_fd2pageno(fd);
    disk_manager_->set_fd2pageno(fd, now_page_no + 1);
    std::lock_guard<std::mutex> lock(g_index_latch);
    g_index_entries[fd_].clear();
}

IxIndexHandle::~IxIndexHandle() {
    delete file_hdr_;
}

std::pair<IxNodeHandle *, bool> IxIndexHandle::find_leaf_page(const char *key, Operation operation,
                                                              Transaction *transaction, bool find_first) {
    (void)key;
    (void)operation;
    (void)transaction;
    (void)find_first;
    return {nullptr, false};
}

bool IxIndexHandle::get_value(const char *key, std::vector<Rid> *result, Transaction *transaction) {
    (void)transaction;
    std::lock_guard<std::mutex> lock(g_index_latch);
    result->clear();
    auto &entries = g_index_entries[fd_];
    auto it = lower_entry(file_hdr_, entries, key);
    if (it == entries.end() || compare_key(file_hdr_, it->first, key) != 0) {
        return false;
    }
    result->push_back(it->second);
    return true;
}

void IxIndexHandle::get_range(const char *lower_key, bool lower_inclusive, bool has_lower,
                              const char *upper_key, bool upper_inclusive, bool has_upper,
                              std::vector<Rid> *result) const {
    std::lock_guard<std::mutex> lock(g_index_latch);
    result->clear();
    const auto &entries = g_index_entries[fd_];
    auto it = has_lower ? lower_entry(file_hdr_, entries, lower_key) : entries.begin();
    if (has_lower && !lower_inclusive) {
        while (it != entries.end() && compare_key(file_hdr_, it->first, lower_key) == 0) {
            ++it;
        }
    }
    for (; it != entries.end(); ++it) {
        if (has_upper) {
            int cmp = compare_key(file_hdr_, it->first, upper_key);
            if (cmp > 0 || (!upper_inclusive && cmp == 0)) {
                break;
            }
        }
        result->push_back(it->second);
    }
}

IxNodeHandle *IxIndexHandle::split(IxNodeHandle *node) {
    return node;
}

void IxIndexHandle::insert_into_parent(IxNodeHandle *old_node, const char *key, IxNodeHandle *new_node,
                                       Transaction *transaction) {
    (void)old_node;
    (void)key;
    (void)new_node;
    (void)transaction;
}

page_id_t IxIndexHandle::insert_entry(const char *key, const Rid &value, Transaction *transaction) {
    (void)transaction;
    std::lock_guard<std::mutex> lock(g_index_latch);
    auto &entries = g_index_entries[fd_];
    auto it = lower_entry(file_hdr_, entries, key);
    if (it != entries.end() && compare_key(file_hdr_, it->first, key) == 0) {
        throw InternalError("Duplicate index key");
    }
    std::vector<char> key_buf(key, key + file_hdr_->col_tot_len_);
    entries.insert(it, {std::move(key_buf), value});
    return IX_INIT_ROOT_PAGE;
}

bool IxIndexHandle::delete_entry(const char *key, Transaction *transaction) {
    (void)transaction;
    std::lock_guard<std::mutex> lock(g_index_latch);
    auto &entries = g_index_entries[fd_];
    auto it = lower_entry(file_hdr_, entries, key);
    if (it == entries.end() || compare_key(file_hdr_, it->first, key) != 0) {
        return false;
    }
    entries.erase(it);
    return true;
}

bool IxIndexHandle::coalesce_or_redistribute(IxNodeHandle *node, Transaction *transaction, bool *root_is_latched) {
    (void)node;
    (void)transaction;
    (void)root_is_latched;
    return false;
}

bool IxIndexHandle::adjust_root(IxNodeHandle *old_root_node) {
    (void)old_root_node;
    return false;
}

void IxIndexHandle::redistribute(IxNodeHandle *neighbor_node, IxNodeHandle *node, IxNodeHandle *parent, int index) {
    (void)neighbor_node;
    (void)node;
    (void)parent;
    (void)index;
}

bool IxIndexHandle::coalesce(IxNodeHandle **neighbor_node, IxNodeHandle **node, IxNodeHandle **parent, int index,
                             Transaction *transaction, bool *root_is_latched) {
    (void)neighbor_node;
    (void)node;
    (void)parent;
    (void)index;
    (void)transaction;
    (void)root_is_latched;
    return false;
}

Rid IxIndexHandle::get_rid(const Iid &iid) const {
    std::lock_guard<std::mutex> lock(g_index_latch);
    const auto &entries = g_index_entries[fd_];
    if (iid.slot_no < 0 || iid.slot_no >= static_cast<int>(entries.size())) {
        throw IndexEntryNotFoundError();
    }
    return entries[iid.slot_no].second;
}

Iid IxIndexHandle::lower_bound(const char *key) {
    std::lock_guard<std::mutex> lock(g_index_latch);
    auto &entries = g_index_entries[fd_];
    auto it = lower_entry(file_hdr_, entries, key);
    return Iid{IX_INIT_ROOT_PAGE, static_cast<int>(std::distance(entries.begin(), it))};
}

Iid IxIndexHandle::upper_bound(const char *key) {
    std::lock_guard<std::mutex> lock(g_index_latch);
    auto &entries = g_index_entries[fd_];
    auto it = lower_entry(file_hdr_, entries, key);
    while (it != entries.end() && compare_key(file_hdr_, it->first, key) == 0) {
        ++it;
    }
    return Iid{IX_INIT_ROOT_PAGE, static_cast<int>(std::distance(entries.begin(), it))};
}

Iid IxIndexHandle::leaf_end() const {
    std::lock_guard<std::mutex> lock(g_index_latch);
    return Iid{IX_INIT_ROOT_PAGE, static_cast<int>(g_index_entries[fd_].size())};
}

Iid IxIndexHandle::leaf_begin() const {
    return Iid{IX_INIT_ROOT_PAGE, 0};
}

IxNodeHandle *IxIndexHandle::fetch_node(int page_no) const {
    Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
    return new IxNodeHandle(file_hdr_, page);
}

IxNodeHandle *IxIndexHandle::create_node() {
    PageId new_page_id = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *page = buffer_pool_manager_->new_page(&new_page_id);
    file_hdr_->num_pages_++;
    return new IxNodeHandle(file_hdr_, page);
}

void IxIndexHandle::maintain_parent(IxNodeHandle *node) {
    (void)node;
}

void IxIndexHandle::erase_leaf(IxNodeHandle *leaf) {
    (void)leaf;
}

void IxIndexHandle::release_node_handle(IxNodeHandle &node) {
    if (node.page == nullptr) {
        return;
    }

    PageId page_id = node.page->get_page_id();
    buffer_pool_manager_->unpin_page(page_id, false);
    delete &node;
}

void IxIndexHandle::maintain_child(IxNodeHandle *node, int child_idx) {
    (void)node;
    (void)child_idx;
}
