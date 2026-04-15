#pragma once

#include <cstdint>
#include <limits>
#include <string>

namespace slang::ast {
class Symbol;
class InstanceBodySymbol;
}

namespace rtl_trace {

struct SignalCompileItem {
  std::string path;
  const slang::ast::Symbol *sym = nullptr;
  const slang::ast::InstanceBodySymbol *body = nullptr;
  // Level 2: struct member metadata (zeroed for non-member signals)
  uint32_t parent_signal_idx = std::numeric_limits<uint32_t>::max();
  uint64_t member_bit_offset = 0;
  uint64_t member_bit_width = 0;
  int struct_depth = 0;  // 0 = original signal, 1 = top-level field, 2 = nested field
};

} // namespace rtl_trace
