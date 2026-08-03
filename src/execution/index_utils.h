#pragma once

#include <cstring>
#include <memory>

#include "system/sm_meta.h"

inline std::unique_ptr<char[]> make_index_key(const IndexMeta &index, const char *record_data) {
    auto key = std::make_unique<char[]>(index.col_tot_len);
    int offset = 0;
    for (auto &col : index.cols) {
        memcpy(key.get() + offset, record_data + col.offset, col.len);
        offset += col.len;
    }
    return key;
}
