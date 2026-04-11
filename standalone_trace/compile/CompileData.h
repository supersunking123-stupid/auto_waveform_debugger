#pragma once

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
};

} // namespace rtl_trace
