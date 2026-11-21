/**
 * @file        core/reserved_core.cpp
 * @brief       Chooses one physical core for a single thread and the CPU sets left to the rest
 *              of the process.
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/reserved_core.h>

#include <algorithm>
#include <map>
#include <utility>

namespace rex::thread {

namespace {

constexpr size_t kMinCores = 4;

using CoreKey = std::pair<uint16_t, uint8_t>;

struct CacheGroup {
  uint64_t l3_bytes = 0;
  size_t cores = 0;
};

}  // namespace

CoreChoice ChooseReservedCore(const std::vector<CpuSetEntry>& sets, uint16_t process_group,
                              uint64_t allowed_mask) {
  CoreChoice choice;

  std::vector<CpuSetEntry> allowed;
  for (const auto& set : sets) {
    if (set.group == process_group && set.logical < 64 && (allowed_mask >> set.logical) & 1) {
      allowed.push_back(set);
    }
  }
  if (allowed.empty()) {
    choice.skip_reason = "no CPU sets available to the process";
    return choice;
  }

  std::map<CoreKey, std::vector<const CpuSetEntry*>> cores;
  for (const auto& set : allowed) cores[{set.group, set.core}].push_back(&set);
  if (cores.size() < kMinCores) {
    choice.skip_reason = std::to_string(cores.size()) + " physical cores, need 4";
    return choice;
  }

  uint8_t fastest = 0;
  for (const auto& set : allowed) fastest = std::max(fastest, set.efficiency);
  std::vector<CoreKey> candidates;
  for (const auto& [key, members] : cores) {
    if (members.front()->efficiency == fastest) candidates.push_back(key);
  }
  if (candidates.size() < kMinCores) {
    choice.skip_reason =
        std::to_string(candidates.size()) + " cores in the fastest class, need 4";
    return choice;
  }

  std::map<uint8_t, CacheGroup> caches;
  for (const auto& key : candidates) {
    for (const auto* set : cores[key]) {
      auto& cache = caches[set->cache];
      cache.l3_bytes = std::max(cache.l3_bytes, set->l3_bytes);
    }
    ++caches[cores[key].front()->cache].cores;
  }
  auto best_cache = caches.begin();
  for (auto it = caches.begin(); it != caches.end(); ++it) {
    if (it->second.l3_bytes > best_cache->second.l3_bytes ||
        (it->second.l3_bytes == best_cache->second.l3_bytes &&
         it->second.cores > best_cache->second.cores)) {
      best_cache = it;
    }
  }

  CoreKey chosen{};
  bool found = false;
  for (const auto& key : candidates) {
    if (cores[key].front()->cache == best_cache->first && (!found || key > chosen)) {
      chosen = key;
      found = true;
    }
  }

  const auto& members = cores[chosen];
  const auto* own = *std::min_element(members.begin(), members.end(),
                                      [](const auto* a, const auto* b) {
                                        return a->logical < b->logical;
                                      });
  for (const auto& set : allowed) {
    if (CoreKey{set.group, set.core} != chosen) choice.others.push_back(set.id);
  }
  choice.reserved = true;
  choice.own = own->id;
  choice.core = own->core;
  choice.cache = own->cache;
  choice.efficiency = own->efficiency;
  choice.l3_bytes = best_cache->second.l3_bytes;
  return choice;
}

}  // namespace rex::thread
