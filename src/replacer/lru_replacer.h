/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <list>
#include <mutex>
#include <unordered_map>

#include "common/config.h"
#include "replacer/replacer.h"

class LRUReplacer : public Replacer {
   public:
    explicit LRUReplacer(size_t num_pages);
    ~LRUReplacer();

    bool victim(frame_id_t *frame_id) override;
    void pin(frame_id_t frame_id) override;
    void unpin(frame_id_t frame_id) override;
    size_t Size() override;

   private:
    std::mutex latch_;
    std::list<frame_id_t> LRUlist_;
    std::unordered_map<frame_id_t, std::list<frame_id_t>::iterator> LRUhash_;
    size_t max_size_;
};
