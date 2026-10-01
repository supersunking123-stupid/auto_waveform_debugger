#pragma once
#include "db/GraphDbTypes.h"

namespace rtl_trace {
// Recheck offsets at access time. This hardens same-size in-place corruption;
// it cannot make an uncooperative concurrent file truncation safe from SIGBUS.
inline std::string_view ReadMappedGraphString(const GraphDb &db, uint32_t id) {
  if (id + uint64_t{1} >= db.mapped_string_offsets.size()) return {};
  const auto base = reinterpret_cast<uintptr_t>(db.mapping.get());
  const auto table = reinterpret_cast<uintptr_t>(db.mapped_string_offsets.data());
  const auto blob = reinterpret_cast<uintptr_t>(db.mapped_string_blob);
  const auto offset_bytes = (uint64_t{id} + 2) * sizeof(uint32_t);
  if (table < base || table - base > db.mapping_bytes || offset_bytes > db.mapping_bytes - (table - base)) return {};
  const uint32_t begin = db.mapped_string_offsets[id];
  const uint32_t end = db.mapped_string_offsets[size_t(id) + 1];
  if (end < begin || end > db.mapped_string_blob_bytes || blob < base ||
      blob - base > db.mapping_bytes || end > db.mapping_bytes - (blob - base)) return {};
  return {db.mapped_string_blob + begin, end - begin};
}
} // namespace rtl_trace
