#pragma once

#include "db/GraphDbTypes.h"

#include <functional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace rtl_trace {

// Compile-time equality is deliberately stricter than query EndpointKey.
// Keep IDs, string refs, vector order/multiplicity, and coordinate flags distinct.
inline bool SameEndpointFields(const EndpointRecord &a, const EndpointRecord &b) {
  return a.kind == b.kind && a.path == b.path && a.file == b.file && a.path_id == b.path_id &&
         a.file_id == b.file_id && a.line == b.line && a.direction == b.direction &&
         a.assignment_text == b.assignment_text && a.has_assignment_range == b.has_assignment_range &&
         a.assignment_start == b.assignment_start && a.assignment_end == b.assignment_end &&
         a.bit_map == b.bit_map && a.bit_map_approximate == b.bit_map_approximate &&
         a.bit_map_logical_axes == b.bit_map_logical_axes && a.bit_map_merged == b.bit_map_merged &&
         a.lhs_signal_ids == b.lhs_signal_ids && a.rhs_signal_ids == b.rhs_signal_ids &&
         a.lhs_signals == b.lhs_signals && a.rhs_signals == b.rhs_signals;
}

struct EndpointFullKey {
  const EndpointRecord *endpoint;
  bool operator==(const EndpointFullKey &other) const {
    return SameEndpointFields(*endpoint, *other.endpoint);
  }
};

struct EndpointFullKeyHash {
  size_t operator()(const EndpointFullKey &key) const {
    const EndpointRecord &e = *key.endpoint;
    size_t h = 0;
    auto value = [&](const auto &v) {
      using T = std::decay_t<decltype(v)>;
      const size_t next = std::hash<T>{}(v);
      h ^= next + 0x9e3779b9u + (h << 6) + (h >> 2);
    };
    auto refs = [&](const auto &v) {
      value(v.size());
      for (const auto &item : v) value(item);
    };
    value(e.kind);
    value(e.path); value(e.path_id);
    value(e.file); value(e.file_id);
    value(e.line); value(e.direction);
    value(e.assignment_text); value(e.has_assignment_range);
    value(e.assignment_start); value(e.assignment_end);
    value(e.bit_map); value(e.bit_map_approximate);
    value(e.bit_map_logical_axes); value(e.bit_map_merged);
    refs(e.lhs_signal_ids); refs(e.rhs_signal_ids);
    refs(e.lhs_signals); refs(e.rhs_signals);
    return h;
  }
};

template <typename Hash = EndpointFullKeyHash>
struct EndpointDedupScratch {
  static constexpr size_t kEmpty = std::numeric_limits<size_t>::max();
  struct Slot {
    size_t index = kEmpty;
    size_t hash = 0;
  };
  std::vector<Slot> slots;
  std::vector<size_t> touched;
  std::vector<uint8_t> dropped;
  Hash hash;

  void ClearTouched() {
    for (size_t slot : touched) slots[slot].index = kEmpty;
    touched.clear();
  }

  void Release() {
    slots = decltype(slots){};
    touched = decltype(touched){};
    dropped = decltype(dropped){};
    hash = Hash{};
  }
};

// All lookups finish against stable original records. Clear original indices before
// moving survivors, preserving their first occurrence and source order.
template <typename Hash>
size_t DeduplicateEndpointsInPlace(std::vector<EndpointRecord> &endpoints,
                                   EndpointDedupScratch<Hash> &scratch) {
  // Most signal lists have zero or one entry. Do not visit a retained table's
  // capacity, or touch any scratch state, for those lists.
  if (endpoints.size() < 2) return 0;
  scratch.ClearTouched();
  if (scratch.slots.size() / 2 < endpoints.size()) {
    size_t capacity = 8;
    while (capacity / 2 < endpoints.size()) {
      if (capacity > scratch.slots.max_size() / 2)
        throw std::length_error("endpoint dedup table capacity");
      capacity *= 2;
    }
    scratch.slots.assign(capacity, {});
  }
  scratch.dropped.assign(endpoints.size(), 0);
  const size_t mask = scratch.slots.size() - 1;
  size_t removed = 0;
  for (size_t i = 0; i < endpoints.size(); ++i) {
    const size_t hash = scratch.hash(EndpointFullKey{&endpoints[i]});
    size_t position = hash & mask;
    while (scratch.slots[position].index != EndpointDedupScratch<Hash>::kEmpty) {
      const auto &slot = scratch.slots[position];
      if (slot.hash == hash && SameEndpointFields(endpoints[slot.index], endpoints[i])) {
        scratch.dropped[i] = 1;
        ++removed;
        break;
      }
      position = (position + 1) & mask;
    }
    if (!scratch.dropped[i]) {
      scratch.touched.push_back(position);
      scratch.slots[position] = {i, hash};
    }
  }
  // Indices refer to the original stable records until every lookup is done.
  // Clear only occupied slots before any survivor moves.
  scratch.ClearTouched();
  if (removed == 0) return 0;
  size_t out = 0;
  for (size_t i = 0; i < endpoints.size(); ++i) {
    if (scratch.dropped[i]) continue;
    if (out != i) endpoints[out] = std::move(endpoints[i]);
    ++out;
  }
  endpoints.resize(out);
  return removed;
}

}  // namespace rtl_trace
