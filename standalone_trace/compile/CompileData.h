#pragma once

#include <cstdint>
#include <limits>
#include <string>

namespace slang::ast {
class Symbol;
class InstanceBodySymbol;
}

namespace rtl_trace {

// One traceable signal. Its hierarchical path is not stored here but in a parallel
// std::vector<std::string> (same index) that SaveGraphDb releases as a whole once the paths are
// interned; keeping it out of this struct saves the 32-byte string header per signal for the
// whole build loop. Fields are ordered to avoid padding (40 bytes).
struct SignalCompileItem {
  const slang::ast::Symbol *sym = nullptr;
  const slang::ast::InstanceBodySymbol *body = nullptr;
  // Level 2: struct member metadata (zeroed for non-member signals)
  uint64_t member_bit_offset = 0;
  uint64_t member_bit_width = 0;
  uint32_t parent_signal_idx = std::numeric_limits<uint32_t>::max();
  int struct_depth = 0;  // 0 = original signal, 1 = top-level field, 2 = nested field
};

} // namespace rtl_trace
