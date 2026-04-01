// GraphDb.cc — Shared infrastructure for standalone_trace.
// Contains: compile-time trace building, graph DB I/O, session management,
// string/path/bit utilities, CLI helpers, help printers, and query output functions.

#include "db/GraphDb.h"
#include "db/GraphDbTypes.h"
#include "db/GraphDbInternals.h"
#include "AssignmentUtils.h"

// Slang AST headers needed for compile-time trace building
#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/EvalContext.h"
#include "slang/ast/Expression.h"
#include "slang/ast/Scope.h"
#include "slang/ast/Symbol.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/ast/expressions/AssignmentExpressions.h"
#include "slang/ast/expressions/LiteralExpressions.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/statements/MiscStatements.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/driver/Driver.h"
#include "slang/text/SourceManager.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/resource.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace rtl_trace {

// --- Memory utilities (global namespace in original, now inside rtl_trace) ---

long GetMaxRSSMB() {
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    return usage.ru_maxrss / 1024; // Linux ru_maxrss is in kb
  }
  return 0;
}

long GetCurrentRSSMB() {
  long rss = 0;
  std::ifstream in("/proc/self/statm");
  if (in.is_open()) {
    long size, resident;
    if (in >> size >> resident) {
      long page_size = sysconf(_SC_PAGE_SIZE);
      rss = (resident * page_size) / (1024 * 1024);
    }
  }
  return rss;
}

void LogMem(const std::string& step) {
  std::cout << "[Memory] " << step << " Current RSS: " << GetCurrentRSSMB() << "MB, Peak RSS: " << GetMaxRSSMB() << "MB\n";
}

// --- Compile-time-only types (not in GraphDbTypes.h because they depend on slang AST) ---

using SymbolRefList = std::vector<const slang::ast::Symbol *>;

struct ExprTraceResult {
  const slang::ast::Expression *expr = nullptr;
  const slang::ast::Symbol *symbol = nullptr;
  const slang::ast::AssignmentExpression *assignment = nullptr;
  std::vector<const slang::ast::Expression *> selectors;
  SymbolRefList context_lhs_signals;
  bool context_from_instance_port = false;
  const slang::ast::InstanceSymbol *context_instance = nullptr;
  const slang::ast::PortSymbol *context_port = nullptr;
};

using TraceResult = std::variant<const slang::ast::PortSymbol *, ExprTraceResult>;

struct BodyTraceIndex {
  slang::flat_hash_map<const slang::ast::Symbol *, std::vector<TraceResult>> drivers;
  slang::flat_hash_map<const slang::ast::Symbol *, std::vector<TraceResult>> loads;
};

struct TraceCompileCache {
  slang::flat_hash_map<const slang::ast::AssignmentExpression *, SymbolRefList> assignment_lhs_signals;
  slang::flat_hash_map<const slang::ast::AssignmentExpression *, SymbolRefList> assignment_rhs_signals;
  slang::flat_hash_map<const slang::ast::Statement *, SymbolRefList> statement_lhs_signals;
  slang::flat_hash_map<const slang::ast::InstanceBodySymbol *, BodyTraceIndex> body_trace_indexes;
};

// ScopedFileLock — local utility for SaveGraphDb file locking
class ScopedFileLock {
 public:
  ScopedFileLock() = default;
  ScopedFileLock(const ScopedFileLock &) = delete;
  ScopedFileLock &operator=(const ScopedFileLock &) = delete;

  ~ScopedFileLock() { Release(); }

  bool Acquire(const std::string &path) {
    Release();
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0666);
    if (fd_ < 0) return false;
    if (::flock(fd_, LOCK_EX) != 0) {
      Release();
      return false;
    }
    return true;
  }

  void Release() {
    if (fd_ >= 0) {
      ::flock(fd_, LOCK_UN);
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  int fd_ = -1;
};

// --- Compile-time forward declarations (defined later in this file) ---

SymbolRefList CollectLhsSignalsFromStatement(const slang::ast::Statement &stmt);
const SymbolRefList &GetCachedLhsSignals(
    const slang::ast::AssignmentExpression *assignment, TraceCompileCache &cache);
const SymbolRefList &GetCachedRhsSignals(
    const slang::ast::AssignmentExpression *assignment, TraceCompileCache &cache);
const SymbolRefList &GetCachedStatementLhsSignals(
    const slang::ast::Statement &stmt, TraceCompileCache &cache);
EndpointRecord ResolveTraceResult(const TraceResult &r, const slang::SourceManager &sm,
                                  bool drivers_mode, TraceCompileCache *cache);
const slang::ast::InstanceBodySymbol *GetContainingInstance(const slang::ast::Symbol *sym) {
  while (sym != nullptr && sym->kind != slang::ast::SymbolKind::InstanceBody) {
    if (sym->kind == slang::ast::SymbolKind::Root) return nullptr;
    sym = &sym->getHierarchicalParent()->asSymbol();
  }
  return sym->as_if<slang::ast::InstanceBodySymbol>();
}

const slang::ast::InstanceSymbol *GetContainingInstanceSymbol(const slang::ast::Symbol *sym) {
  while (sym != nullptr && sym->kind != slang::ast::SymbolKind::Instance) {
    if (sym->kind == slang::ast::SymbolKind::Root) return nullptr;
    sym = &sym->getHierarchicalParent()->asSymbol();
  }
  return sym->as_if<slang::ast::InstanceSymbol>();
}

bool IsTraceable(const slang::ast::Symbol *sym) {
  if (sym == nullptr) return false;
  return sym->kind == slang::ast::SymbolKind::Net || sym->kind == slang::ast::SymbolKind::Variable;
}

bool SymbolPathLess(const slang::ast::Symbol *lhs, const slang::ast::Symbol *rhs) {
  if (lhs == rhs) return false;
  return std::string(lhs->getHierarchicalPath()) < std::string(rhs->getHierarchicalPath());
}

std::vector<std::string> MaterializeSignalPaths(const SymbolRefList &signals) {
  std::vector<std::string> out;
  out.reserve(signals.size());
  for (const slang::ast::Symbol *sym : signals) {
    if (sym == nullptr) continue;
    out.push_back(std::string(sym->getHierarchicalPath()));
  }
  return out;
}

std::string MakeInstancePortPath(const slang::ast::InstanceSymbol *instance,
                                 const slang::ast::PortSymbol *port) {
  if (instance == nullptr || port == nullptr) return "";
  return std::string(instance->getHierarchicalPath()) + "." + std::string(port->name);
}

template <bool DRIVERS>
class BodyTraceIndexBuilder : public slang::ast::ASTVisitor<BodyTraceIndexBuilder<DRIVERS>, slang::ast::VisitFlags::AllGood> {
 public:
  BodyTraceIndexBuilder(BodyTraceIndex &index, TraceCompileCache &cache)
      : index_(index), cache_(cache) {}

  void handle(const slang::ast::InstanceSymbol &inst) {
    for (const slang::ast::PortConnection *conn : inst.getPortConnections()) {
      const auto *port = conn->port.template as_if<slang::ast::PortSymbol>();
      if (port == nullptr) continue;
      if (port->direction ==
          (DRIVERS ? slang::ast::ArgumentDirection::In : slang::ast::ArgumentDirection::Out)) {
        continue;
      }
      const slang::ast::Expression *expr = conn->getExpression();
      if (expr == nullptr) continue;
      checking_instance_port_expression_ = true;
      active_instance_ = &inst;
      active_port_ = port;
      expr->visit(*this);
      active_port_ = nullptr;
      active_instance_ = nullptr;
      checking_instance_port_expression_ = false;
    }
  }

  void handle(const slang::ast::PortSymbol &port) {
    if (port.direction ==
        (DRIVERS ? slang::ast::ArgumentDirection::Out : slang::ast::ArgumentDirection::In)) {
      return;
    }
    if (port.internalSymbol == nullptr || !IsTraceable(port.internalSymbol)) return;
    Entries()[port.internalSymbol].push_back(&port);
  }

  void handle(const slang::ast::AssignmentExpression &assignment) {
    const slang::ast::AssignmentExpression *saved_assignment = current_assignment_;
    current_assignment_ = &assignment;
    checking_lhs_ = true;
    assignment.left().visit(*this);
    checking_lhs_ = false;
    checking_rhs_ = true;
    assignment.right().visit(*this);
    checking_rhs_ = false;
    current_assignment_ = saved_assignment;
  }

  void handle(const slang::ast::ConditionalStatement &stmt) {
    if constexpr (DRIVERS) {
      this->visitDefault(stmt);
      return;
    }
    SymbolRefList context_lhs = GetCachedStatementLhsSignals(stmt.ifTrue, cache_);
    if (stmt.ifFalse != nullptr) {
      const auto &else_lhs = GetCachedStatementLhsSignals(*stmt.ifFalse, cache_);
      context_lhs.insert(context_lhs.end(), else_lhs.begin(), else_lhs.end());
      std::sort(context_lhs.begin(), context_lhs.end(), SymbolPathLess);
      context_lhs.erase(std::unique(context_lhs.begin(), context_lhs.end()), context_lhs.end());
    }

    condition_lhs_stack_.push_back(std::move(context_lhs));
    const slang::ast::Expression *saved_condition = current_condition_expr_;
    for (const auto &cond : stmt.conditions) {
      current_condition_expr_ = cond.expr;
      cond.expr->visit(*this);
    }
    current_condition_expr_ = saved_condition;

    stmt.ifTrue.visit(*this);
    if (stmt.ifFalse != nullptr) stmt.ifFalse->visit(*this);
    condition_lhs_stack_.pop_back();
  }

  void handle(const slang::ast::CaseStatement &stmt) {
    if constexpr (DRIVERS) {
      this->visitDefault(stmt);
      return;
    }
    SymbolRefList context_lhs;
    for (const auto &item : stmt.items) {
      const auto &item_lhs = GetCachedStatementLhsSignals(*item.stmt, cache_);
      context_lhs.insert(context_lhs.end(), item_lhs.begin(), item_lhs.end());
    }
    if (stmt.defaultCase != nullptr) {
      const auto &default_lhs = GetCachedStatementLhsSignals(*stmt.defaultCase, cache_);
      context_lhs.insert(context_lhs.end(), default_lhs.begin(), default_lhs.end());
    }
    std::sort(context_lhs.begin(), context_lhs.end(), SymbolPathLess);
    context_lhs.erase(std::unique(context_lhs.begin(), context_lhs.end()), context_lhs.end());

    condition_lhs_stack_.push_back(std::move(context_lhs));
    const slang::ast::Expression *saved_condition = current_condition_expr_;
    current_condition_expr_ = &stmt.expr;
    stmt.expr.visit(*this);
    current_condition_expr_ = saved_condition;

    for (const auto &item : stmt.items)
      item.stmt->visit(*this);
    if (stmt.defaultCase != nullptr) stmt.defaultCase->visit(*this);
    condition_lhs_stack_.pop_back();
  }

  void handle(const slang::ast::TimedStatement &stmt) {
    if constexpr (DRIVERS) {
      this->visitDefault(stmt);
      return;
    }

    timed_lhs_stack_.push_back(GetCachedStatementLhsSignals(stmt.stmt, cache_));
    stmt.timing.visit(*this);
    timed_lhs_stack_.pop_back();
    stmt.stmt.visit(*this);
  }

  void handle(const slang::ast::RangeSelectExpression &expr) {
    selector_stack_.push_back(&expr);
    expr.value().visit(*this);
    selector_stack_.pop_back();
    if constexpr (!DRIVERS) {
      selector_depth_++;
      expr.left().visit(*this);
      expr.right().visit(*this);
      selector_depth_--;
    }
  }

  void handle(const slang::ast::ElementSelectExpression &expr) {
    selector_stack_.push_back(&expr);
    expr.value().visit(*this);
    selector_stack_.pop_back();
    if constexpr (!DRIVERS) {
      selector_depth_++;
      expr.selector().visit(*this);
      selector_depth_--;
    }
  }

  void handle(const slang::ast::NamedValueExpression &nve) {
    if (!IsTraceable(&nve.symbol)) return;
    if constexpr (DRIVERS) {
      if (checking_instance_port_expression_ || checking_lhs_) {
        ExprTraceResult result;
        result.expr = &nve;
        result.symbol = &nve.symbol;
        if (checking_lhs_) result.assignment = current_assignment_;
        result.selectors = selector_stack_;
        if (checking_instance_port_expression_ && active_instance_ != nullptr && active_port_ != nullptr) {
          result.context_from_instance_port = true;
          result.context_instance = active_instance_;
          result.context_port = active_port_;
        }
        Entries()[&nve.symbol].push_back(std::move(result));
      }
    } else {
      if (!(checking_lhs_ && selector_depth_ == 0)) {
        ExprTraceResult result;
        result.expr = &nve;
        result.symbol = &nve.symbol;
        result.assignment = current_assignment_;
        result.selectors = selector_stack_;
        if (checking_instance_port_expression_ && active_instance_ != nullptr && active_port_ != nullptr) {
          result.context_from_instance_port = true;
          result.context_instance = active_instance_;
          result.context_port = active_port_;
        }
        if (current_assignment_ == nullptr && current_condition_expr_ != nullptr &&
            !condition_lhs_stack_.empty()) {
          result.context_lhs_signals = condition_lhs_stack_.back();
        } else if (current_assignment_ == nullptr && !timed_lhs_stack_.empty()) {
          result.context_lhs_signals = timed_lhs_stack_.back();
        }
        Entries()[&nve.symbol].push_back(std::move(result));
      }
    }
  }

  void handle(const slang::ast::MemberAccessExpression &expr) {
    const slang::ast::Symbol *sym = expr.getSymbolReference();
    if (!IsTraceable(sym)) return;
    if constexpr (DRIVERS) {
      if (checking_instance_port_expression_ || checking_lhs_) {
        ExprTraceResult result;
        result.expr = &expr;
        result.symbol = sym;
        if (checking_lhs_) result.assignment = current_assignment_;
        result.selectors = selector_stack_;
        if (checking_instance_port_expression_ && active_instance_ != nullptr && active_port_ != nullptr) {
          result.context_from_instance_port = true;
          result.context_instance = active_instance_;
          result.context_port = active_port_;
        }
        Entries()[sym].push_back(std::move(result));
      }
    } else {
      if (!(checking_lhs_ && selector_depth_ == 0)) {
        ExprTraceResult result;
        result.expr = &expr;
        result.symbol = sym;
        result.assignment = current_assignment_;
        result.selectors = selector_stack_;
        if (checking_instance_port_expression_ && active_instance_ != nullptr && active_port_ != nullptr) {
          result.context_from_instance_port = true;
          result.context_instance = active_instance_;
          result.context_port = active_port_;
        }
        if (current_assignment_ == nullptr && current_condition_expr_ != nullptr &&
            !condition_lhs_stack_.empty()) {
          result.context_lhs_signals = condition_lhs_stack_.back();
        } else if (current_assignment_ == nullptr && !timed_lhs_stack_.empty()) {
          result.context_lhs_signals = timed_lhs_stack_.back();
        }
        Entries()[sym].push_back(std::move(result));
      }
    }
  }

  void handle(const slang::ast::UninstantiatedDefSymbol &uninst) {}

 private:
  slang::flat_hash_map<const slang::ast::Symbol *, std::vector<TraceResult>> &Entries() {
    if constexpr (DRIVERS) {
      return index_.drivers;
    } else {
      return index_.loads;
    }
  }

  BodyTraceIndex &index_;
  TraceCompileCache &cache_;
  const slang::ast::InstanceSymbol *active_instance_ = nullptr;
  const slang::ast::PortSymbol *active_port_ = nullptr;
  bool checking_instance_port_expression_ = false;
  bool checking_lhs_ = false;
  bool checking_rhs_ = false;
  int selector_depth_ = 0;
  const slang::ast::AssignmentExpression *current_assignment_ = nullptr;
  const slang::ast::Expression *current_condition_expr_ = nullptr;
  std::vector<const slang::ast::Expression *> selector_stack_;
  std::vector<SymbolRefList> condition_lhs_stack_;
  std::vector<SymbolRefList> timed_lhs_stack_;
};

const BodyTraceIndex &GetOrBuildBodyTraceIndex(const slang::ast::InstanceBodySymbol &body,
                                               TraceCompileCache &cache) {
  auto it = cache.body_trace_indexes.find(&body);
  if (it != cache.body_trace_indexes.end()) return it->second;
  auto [inserted_it, _] = cache.body_trace_indexes.emplace(&body, BodyTraceIndex{});
  BodyTraceIndexBuilder</*DRIVERS*/ true> driver_builder(inserted_it->second, cache);
  body.visit(driver_builder);
  BodyTraceIndexBuilder</*DRIVERS*/ false> load_builder(inserted_it->second, cache);
  body.visit(load_builder);
  return inserted_it->second;
}

class PortConnectionResultCollector
    : public slang::ast::ASTVisitor<PortConnectionResultCollector, slang::ast::VisitFlags::AllGood> {
 public:
  PortConnectionResultCollector(std::vector<TraceResult> &out,
                                std::unordered_set<const slang::ast::Symbol *> &visited)
      : out_(out), visited_(visited) {}

  void handle(const slang::ast::NamedValueExpression &nve) {
    if (!IsTraceable(&nve.symbol)) return;
    if (!visited_.insert(&nve.symbol).second) return;
    ExprTraceResult result;
    result.expr = &nve;
    result.symbol = &nve.symbol;
    result.assignment = nullptr;
    result.context_from_instance_port = true;
    result.context_instance = instance_;
    result.context_port = port_;
    out_.push_back(std::move(result));
  }

  void handle(const slang::ast::MemberAccessExpression &expr) {
    const slang::ast::Symbol *sym = expr.getSymbolReference();
    if (!IsTraceable(sym)) return;
    if (!visited_.insert(sym).second) return;
    ExprTraceResult result;
    result.expr = &expr;
    result.symbol = sym;
    result.assignment = nullptr;
    result.context_from_instance_port = true;
    result.context_instance = instance_;
    result.context_port = port_;
    out_.push_back(std::move(result));
  }

  void SetActiveInstancePort(const slang::ast::InstanceSymbol *instance,
                             const slang::ast::PortSymbol *port) {
    instance_ = instance;
    port_ = port;
  }

 private:
  std::vector<TraceResult> &out_;
  std::unordered_set<const slang::ast::Symbol *> &visited_;
  const slang::ast::InstanceSymbol *instance_ = nullptr;
  const slang::ast::PortSymbol *port_ = nullptr;
};

std::vector<TraceResult> CollectPortConnectionResults(
    const slang::ast::PortSymbol &port, const slang::ast::Symbol *sym,
    std::unordered_set<const slang::ast::Symbol *> &visited) {
  const slang::ast::Symbol *context = sym;
  if (const auto *sym_port = sym->as_if<slang::ast::PortSymbol>()) {
    if (sym_port->internalSymbol != nullptr) context = sym_port->internalSymbol;
  }
  const slang::ast::InstanceSymbol *inst = GetContainingInstanceSymbol(context);
  if (inst == nullptr) return {};

  std::vector<TraceResult> out;
  for (const slang::ast::PortConnection *conn : inst->getPortConnections()) {
    const auto *conn_port = conn->port.template as_if<slang::ast::PortSymbol>();
    if (conn_port != &port) continue;
    const slang::ast::Expression *expr = conn->getExpression();
    if (expr == nullptr) continue;
    PortConnectionResultCollector collector(out, visited);
    collector.SetActiveInstancePort(inst, conn_port);
    expr->visit(collector);
    break;
  }
  return out;
}

template <bool DRIVERS>
std::vector<TraceResult> ComputeIndexedTraceResults(
    const slang::ast::Symbol *sym, TraceCompileCache &cache,
    std::unordered_set<const slang::ast::Symbol *> &visited) {
  const slang::ast::InstanceBodySymbol *body = GetContainingInstance(sym);
  if (body == nullptr) return {};

  const BodyTraceIndex &index = GetOrBuildBodyTraceIndex(*body, cache);
  const auto &entries = [&]() -> const slang::flat_hash_map<const slang::ast::Symbol *, std::vector<TraceResult>> & {
    if constexpr (DRIVERS) {
      return index.drivers;
    } else {
      return index.loads;
    }
  }();
  const auto it = entries.find(sym);
  if (it == entries.end()) return {};

  std::vector<TraceResult> out;
  out.reserve(it->second.size());
  for (const TraceResult &entry : it->second) {
    if (const auto *port = std::get_if<const slang::ast::PortSymbol *>(&entry)) {
      bool followed = false;
      if constexpr (DRIVERS) {
        if ((*port)->direction == slang::ast::ArgumentDirection::In) {
          std::vector<TraceResult> parent = CollectPortConnectionResults(**port, sym, visited);
          if (!parent.empty()) {
            out.insert(out.end(), std::make_move_iterator(parent.begin()), std::make_move_iterator(parent.end()));
            followed = true;
          }
        }
      } else {
        if ((*port)->direction == slang::ast::ArgumentDirection::Out) {
          std::vector<TraceResult> parent = CollectPortConnectionResults(**port, sym, visited);
          if (!parent.empty()) {
            out.insert(out.end(), std::make_move_iterator(parent.begin()), std::make_move_iterator(parent.end()));
            followed = true;
          }
        }
      }
      if (!followed) out.push_back(*port);
      continue;
    }

    const auto *expr = std::get_if<ExprTraceResult>(&entry);
    bool followed = false;
    if (expr != nullptr && expr->context_from_instance_port && expr->context_port != nullptr &&
        expr->context_port->internalSymbol != nullptr) {
      const slang::ast::Symbol *internal = expr->context_port->internalSymbol;
      if (visited.insert(internal).second) {
        std::vector<TraceResult> nested = ComputeIndexedTraceResults<DRIVERS>(internal, cache, visited);
        if (!nested.empty()) {
          out.insert(out.end(), std::make_move_iterator(nested.begin()), std::make_move_iterator(nested.end()));
          followed = true;
        }
      }
    }
    if (!followed && expr != nullptr) out.push_back(*expr);
  }
  return out;
}

SignalRecord BuildSignalRecord(const slang::ast::Symbol *sym, const slang::SourceManager &sm,
                               TraceCompileCache &cache) {
  SignalRecord rec;
  std::unordered_set<const slang::ast::Symbol *> visited_drivers;
  visited_drivers.insert(sym);
  for (const TraceResult &r : ComputeIndexedTraceResults</*DRIVERS*/ true>(sym, cache, visited_drivers))
    rec.drivers.push_back(ResolveTraceResult(r, sm, true, &cache));

  std::unordered_set<const slang::ast::Symbol *> visited_loads;
  visited_loads.insert(sym);
  for (const TraceResult &r : ComputeIndexedTraceResults</*DRIVERS*/ false>(sym, cache, visited_loads))
    rec.loads.push_back(ResolveTraceResult(r, sm, false, &cache));

  return rec;
}

std::string DirectionToString(slang::ast::ArgumentDirection dir) {
  switch (dir) {
  case slang::ast::ArgumentDirection::In: return "input";
  case slang::ast::ArgumentDirection::Out: return "output";
  case slang::ast::ArgumentDirection::InOut: return "inout";
  case slang::ast::ArgumentDirection::Ref: return "ref";
  }
  return "unknown";
}

std::string GetSourceText(slang::SourceRange range, const slang::SourceManager &sm) {
  if (!range.start().valid() || !range.end().valid()) return "";
  const slang::SourceRange original = sm.getFullyOriginalRange(range);
  const slang::SourceLocation start = original.start();
  const slang::SourceLocation end = original.end();
  if (!start.valid() || !end.valid()) return "";
  if (start.buffer() != end.buffer()) return "";
  if (end.offset() < start.offset()) return "";
  const std::string_view full = sm.getSourceText(start.buffer());
  if (start.offset() >= full.size() || end.offset() > full.size()) return "";
  return std::string(full.substr(start.offset(), end.offset() - start.offset()));
}

std::optional<std::pair<uint32_t, uint32_t>> GetSourceOffsetRange(slang::SourceRange range,
                                                                  const slang::SourceManager &sm) {
  if (!range.start().valid() || !range.end().valid()) return std::nullopt;
  const slang::SourceRange original = sm.getFullyOriginalRange(range);
  const slang::SourceLocation start = original.start();
  const slang::SourceLocation end = original.end();
  if (!start.valid() || !end.valid()) return std::nullopt;
  if (start.buffer() != end.buffer()) return std::nullopt;
  if (end.offset() < start.offset()) return std::nullopt;
  if (end.offset() > std::numeric_limits<uint32_t>::max() ||
      start.offset() > std::numeric_limits<uint32_t>::max())
    return std::nullopt;
  return std::make_pair(static_cast<uint32_t>(start.offset()), static_cast<uint32_t>(end.offset()));
}

std::string_view ParentPath(std::string_view path) {
  const size_t pos = path.rfind('.');
  if (pos == std::string_view::npos) return "";
  return path.substr(0, pos);
}

std::string_view LeafName(std::string_view path) {
  const size_t pos = path.rfind('.');
  if (pos == std::string_view::npos) return path;
  return path.substr(pos + 1);
}

std::pair<std::string_view, std::string_view> SplitPathPrefixLeaf(std::string_view path) {
  const size_t pos = path.rfind('.');
  if (pos == std::string_view::npos) return {"", path};
  return {path.substr(0, pos), path.substr(pos + 1)};
}

uint32_t InternString(const std::string &s, std::vector<std::string> &pool,
                      slang::flat_hash_map<std::string, uint32_t> &index) {
  auto it = index.find(s);
  if (it != index.end()) return it->second;
  const uint32_t id = static_cast<uint32_t>(pool.size());
  pool.push_back(s);
  index.emplace(pool.back(), id);
  return id;
}

const std::string &GraphString(const GraphDb &db, uint32_t id) {
  static const std::string empty;
  if (id >= db.strings.size()) return empty;
  return db.strings[id];
}

const std::string &EndpointPath(const TraceDb &db, const EndpointRecord &e) {
  if (!e.path.empty()) return e.path;
  if (e.path_id < db.path_pool.size()) return db.path_pool[e.path_id];
  static const std::string empty;
  return empty;
}

const std::string &EndpointFile(const TraceDb &db, const EndpointRecord &e) {
  if (!e.file.empty()) return e.file;
  if (e.file_id < db.file_pool.size()) return db.file_pool[e.file_id];
  static const std::string empty;
  return empty;
}

std::vector<std::string> SplitJoinedField(const std::string &field) {
  std::vector<std::string> out;
  if (field.empty()) return out;
  size_t start = 0;
  while (start <= field.size()) {
    const size_t pos = field.find('\n', start);
    if (pos == std::string::npos) {
      out.push_back(field.substr(start));
      break;
    }
    out.push_back(field.substr(start, pos - start));
    start = pos + 1;
  }
  return out;
}

std::optional<std::pair<int32_t, int32_t>> ParseExactBitMapText(std::string_view bit_map) {
  if (bit_map.size() < 3 || bit_map.front() != '[' || bit_map.back() != ']') return std::nullopt;
  std::string inside(bit_map.substr(1, bit_map.size() - 2));
  const size_t colon = inside.find(':');
  try {
    if (colon == std::string::npos) {
      const int32_t v = static_cast<int32_t>(std::stoll(inside));
      return std::make_pair(v, v);
    }
    const int32_t l = static_cast<int32_t>(std::stoll(inside.substr(0, colon)));
    const int32_t r = static_cast<int32_t>(std::stoll(inside.substr(colon + 1)));
    return std::make_pair(l, r);
  } catch (...) { return std::nullopt; }
}

std::string FormatBitRange(int32_t hi, int32_t lo) {
  if (hi == lo) return "[" + std::to_string(hi) + "]";
  return "[" + std::to_string(hi) + ":" + std::to_string(lo) + "]";
}

struct EndpointKeyView {
  int kind;
  std::string_view path;
  std::string_view file;
  int line;
  std::string_view direction;
  bool range;
  int32_t a_start;
  int32_t a_end;
  std::string_view a_text;
  const std::vector<std::string>* lhs;
  const std::vector<std::string>* rhs;

  bool operator==(const EndpointKeyView& o) const {
    return kind == o.kind && path == o.path && file == o.file && line == o.line &&
           direction == o.direction && range == o.range && a_start == o.a_start &&
           a_end == o.a_end && a_text == o.a_text && *lhs == *o.lhs && *rhs == *o.rhs;
  }
};

struct EndpointKeyHash {
  size_t operator()(const EndpointKeyView& k) const {
    size_t h = std::hash<std::string_view>()(k.path);
    h ^= std::hash<int>()(k.line) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<std::string_view>()(k.a_text) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
  }
};

EndpointKeyView MakeEndpointKeyView(const EndpointRecord &e) {
  return EndpointKeyView{
      static_cast<int>(e.kind),
      e.path, e.file, e.line, e.direction,
      e.has_assignment_range, e.assignment_start, e.assignment_end, e.assignment_text,
      &e.lhs_signals, &e.rhs_signals};
}

using EndpointMergeGroups = slang::flat_hash_map<EndpointKeyView, std::vector<std::pair<std::pair<int32_t, int32_t>, EndpointRecord>>, EndpointKeyHash>;

void MergeEndpointBitRangesInPlace(std::vector<EndpointRecord>& endpoints, EndpointMergeGroups& groups) {
  groups.clear();
  std::vector<EndpointRecord> out;
  out.reserve(endpoints.size());
  for (EndpointRecord &e : endpoints) {
    if (e.bit_map.empty() || e.bit_map_approximate) {
      out.push_back(std::move(e));
      continue;
    }
    auto parsed = ParseExactBitMapText(e.bit_map);
    if (!parsed.has_value()) {
      out.push_back(std::move(e));
      continue;
    }
    const int32_t lo = std::min(parsed->first, parsed->second);
    const int32_t hi = std::max(parsed->first, parsed->second);
    groups[MakeEndpointKeyView(e)].push_back({{lo, hi}, std::move(e)});
  }

  for (auto &[_, vec] : groups) {
    std::sort(vec.begin(), vec.end(), [](const auto &a, const auto &b) {
      if (a.first.first != b.first.first) return a.first.first < b.first.first;
      return a.first.second < b.first.second;
    });
    size_t i = 0;
    while (i < vec.size()) {
      int32_t cur_lo = vec[i].first.first;
      int32_t cur_hi = vec[i].first.second;
      EndpointRecord merged = std::move(vec[i].second);
      ++i;
      while (i < vec.size()) {
        const int32_t nxt_lo = vec[i].first.first;
        const int32_t nxt_hi = vec[i].first.second;
        if (nxt_lo > cur_hi + 1) break;
        cur_hi = std::max(cur_hi, nxt_hi);
        ++i;
      }
      merged.bit_map = FormatBitRange(cur_hi, cur_lo);
      merged.bit_map_approximate = false;
      out.push_back(std::move(merged));
    }
  }
  endpoints = std::move(out);
}

constexpr size_t kCompactGlobalNetThreshold = 1024;

bool LooksLikeClockOrResetName(std::string_view path) {
  std::string leaf = std::string(LeafName(path));
  for (char &c : leaf)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (leaf == "clk" || leaf == "clock" || leaf == "rst" || leaf == "reset" || leaf == "rst_n" ||
      leaf == "reset_n" || leaf == "resetn" || leaf == "rstn")
    return true;
  return leaf.find("clk") != std::string::npos || leaf.find("clock") != std::string::npos ||
         leaf.find("rst") != std::string::npos || leaf.find("reset") != std::string::npos;
}

std::string ClassifyGlobalNetCategory(std::string_view path) {
  std::string leaf = std::string(LeafName(path));
  for (char &c : leaf)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (leaf.find("rst") != std::string::npos || leaf.find("reset") != std::string::npos) {
    return "reset";
  }
  return "clock";
}

bool ShouldCompactGlobalNet(std::string_view path, size_t load_count) {
  return load_count >= kCompactGlobalNetThreshold && LooksLikeClockOrResetName(path);
}

std::vector<std::string> ExtractCompactSinkPaths(const std::string &source,
                                                 const std::vector<EndpointRecord> &loads) {
  std::vector<std::string> sinks;
  sinks.reserve(loads.size());
  for (const EndpointRecord &e : loads) {
    if (!e.lhs_signals.empty()) {
      sinks.insert(sinks.end(), e.lhs_signals.begin(), e.lhs_signals.end());
      continue;
    }
    if (!e.path.empty() && e.path != source) sinks.push_back(e.path);
  }
  std::sort(sinks.begin(), sinks.end());
  sinks.erase(std::unique(sinks.begin(), sinks.end()), sinks.end());
  return sinks;
}

bool TryParseSimpleInt(std::string_view s, int64_t &out) {
  std::string t;
  for (char c : s) {
    if (!std::isspace(static_cast<unsigned char>(c))) t.push_back(c);
  }
  if (t.empty()) return false;
  size_t idx = 0;
  bool neg = false;
  if (t[0] == '-') {
    neg = true;
    idx = 1;
  }
  if (idx >= t.size()) return false;
  for (size_t i = idx; i < t.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(t[i]))) return false;
  }
  try {
    long long v = std::stoll(t);
    out = static_cast<int64_t>(v);
    return true;
  } catch (...) { return false; }
}

std::pair<std::string, bool> DescribeBitSelectors(
    const std::vector<const slang::ast::Expression *> &selectors, const slang::SourceManager &sm,
    const slang::ast::Symbol &context_symbol) {
  if (selectors.empty()) return {"", false};
  bool approximate = false;
  std::string bit_map;
  for (const slang::ast::Expression *expr : selectors) {
    slang::ast::EvalContext eval_ctx(context_symbol);
    if (std::optional<slang::ConstantRange> range = expr->evalSelector(eval_ctx, false)) {
      if (range->left == range->right) {
        bit_map += "[" + std::to_string(range->left) + "]";
      } else {
        bit_map += "[" + std::to_string(range->left) + ":" + std::to_string(range->right) + "]";
      }
      continue;
    }
    if (const auto *sel = expr->as_if<slang::ast::ElementSelectExpression>()) {
      const std::string sel_text = GetSourceText(sel->selector().sourceRange, sm);
      int64_t idx = 0;
      if (TryParseSimpleInt(sel_text, idx)) {
        bit_map += "[" + std::to_string(idx) + "]";
      } else {
        bit_map += "[" + sel_text + "]";
        approximate = true;
      }
      continue;
    }
    if (const auto *sel = expr->as_if<slang::ast::RangeSelectExpression>()) {
      const std::string left = GetSourceText(sel->left().sourceRange, sm);
      const std::string right = GetSourceText(sel->right().sourceRange, sm);
      int64_t l = 0, r = 0;
      if (TryParseSimpleInt(left, l) && TryParseSimpleInt(right, r)) {
        bit_map += "[" + std::to_string(l) + ":" + std::to_string(r) + "]";
      } else {
        bit_map += "[" + left + ":" + right + "]";
        approximate = true;
      }
      continue;
    }
    const std::string txt = GetSourceText(expr->sourceRange, sm);
    bit_map += "[" + txt + "]";
    approximate = true;
  }
  return {bit_map, approximate};
}

SymbolRefList CollectRhsSignals(const slang::ast::AssignmentExpression *assignment) {
  if (assignment == nullptr) return {};

  class RhsSignalCollector : public slang::ast::ASTVisitor<RhsSignalCollector, slang::ast::VisitFlags::AllGood> {
   public:
    explicit RhsSignalCollector(SymbolRefList &out) : out_(out) {}

    void handle(const slang::ast::NamedValueExpression &nve) {
      if (!IsTraceable(&nve.symbol)) return;
      out_.push_back(&nve.symbol);
    }

    void handle(const slang::ast::MemberAccessExpression &expr) {
      if (const slang::ast::Symbol *sym = expr.getSymbolReference(); IsTraceable(sym)) out_.push_back(sym);
    }

   private:
    SymbolRefList &out_;
  };

  SymbolRefList paths;
  RhsSignalCollector collector(paths);
  assignment->right().visit(collector);
  std::sort(paths.begin(), paths.end(), SymbolPathLess);
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

SymbolRefList CollectLhsSignals(const slang::ast::AssignmentExpression *assignment) {
  if (assignment == nullptr) return {};

  class LhsSignalCollector : public slang::ast::ASTVisitor<LhsSignalCollector, slang::ast::VisitFlags::AllGood> {
   public:
    explicit LhsSignalCollector(SymbolRefList &out) : out_(out) {}

    void handle(const slang::ast::NamedValueExpression &nve) {
      if (!IsTraceable(&nve.symbol)) return;
      out_.push_back(&nve.symbol);
    }

    void handle(const slang::ast::MemberAccessExpression &expr) {
      if (const slang::ast::Symbol *sym = expr.getSymbolReference(); IsTraceable(sym)) out_.push_back(sym);
    }

   private:
    SymbolRefList &out_;
  };

  SymbolRefList paths;
  LhsSignalCollector collector(paths);
  assignment->left().visit(collector);
  std::sort(paths.begin(), paths.end(), SymbolPathLess);
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

SymbolRefList CollectLhsSignalsFromStatement(const slang::ast::Statement &stmt) {
  class StatementLhsCollector
      : public slang::ast::ASTVisitor<StatementLhsCollector, slang::ast::VisitFlags::AllGood> {
   public:
    explicit StatementLhsCollector(SymbolRefList &out) : out_(out) {}

    void handle(const slang::ast::AssignmentExpression &assignment) {
      class LhsCollector : public slang::ast::ASTVisitor<LhsCollector, slang::ast::VisitFlags::AllGood> {
       public:
        explicit LhsCollector(SymbolRefList &out) : out_(out) {}

        void handle(const slang::ast::NamedValueExpression &nve) {
          if (!IsTraceable(&nve.symbol)) return;
          out_.push_back(&nve.symbol);
        }

        void handle(const slang::ast::MemberAccessExpression &expr) {
          if (const slang::ast::Symbol *sym = expr.getSymbolReference(); IsTraceable(sym)) out_.push_back(sym);
        }

       private:
        SymbolRefList &out_;
      };
      LhsCollector lhs_collector(out_);
      assignment.left().visit(lhs_collector);
      assignment.right().visit(*this);
    }

   private:
    SymbolRefList &out_;
  };

  SymbolRefList paths;
  StatementLhsCollector collector(paths);
  stmt.visit(collector);
  std::sort(paths.begin(), paths.end(), SymbolPathLess);
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

const SymbolRefList &GetCachedLhsSignals(
    const slang::ast::AssignmentExpression *assignment, TraceCompileCache &cache) {
  static const SymbolRefList empty;
  if (assignment == nullptr) return empty;
  auto it = cache.assignment_lhs_signals.find(assignment);
  if (it != cache.assignment_lhs_signals.end()) return it->second;
  return cache.assignment_lhs_signals.emplace(assignment, CollectLhsSignals(assignment)).first->second;
}

const SymbolRefList &GetCachedRhsSignals(
    const slang::ast::AssignmentExpression *assignment, TraceCompileCache &cache) {
  static const SymbolRefList empty;
  if (assignment == nullptr) return empty;
  auto it = cache.assignment_rhs_signals.find(assignment);
  if (it != cache.assignment_rhs_signals.end()) return it->second;
  return cache.assignment_rhs_signals.emplace(assignment, CollectRhsSignals(assignment)).first->second;
}

const SymbolRefList &GetCachedStatementLhsSignals(
    const slang::ast::Statement &stmt, TraceCompileCache &cache) {
  auto it = cache.statement_lhs_signals.find(&stmt);
  if (it != cache.statement_lhs_signals.end()) return it->second;
  return cache.statement_lhs_signals.emplace(&stmt, CollectLhsSignalsFromStatement(stmt)).first->second;
}

EndpointRecord ResolveTraceResult(const TraceResult &r, const slang::SourceManager &sm, bool drivers_mode,
                                  TraceCompileCache *cache) {
  EndpointRecord rec;
  std::visit(
      [&](const auto &item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, const slang::ast::PortSymbol *>) {
          rec.kind = EndpointKind::kPort;
          rec.path = item->getHierarchicalPath();
          rec.direction = DirectionToString(item->direction);
          const auto loc = item->location;
          rec.file = std::string(sm.getFileName(loc));
          rec.line = sm.getLineNumber(loc);
        } else {
          rec.kind = EndpointKind::kExpr;
          rec.path = item.symbol != nullptr ? item.symbol->getHierarchicalPath() : "";
          const auto loc = item.expr->sourceRange.start();
          rec.file = std::string(sm.getFileName(loc));
          rec.line = sm.getLineNumber(loc);
          if (item.symbol != nullptr) {
            auto bit_desc = DescribeBitSelectors(item.selectors, sm, *item.symbol);
            rec.bit_map = std::move(bit_desc.first);
            rec.bit_map_approximate = bit_desc.second;
          }
          if (item.assignment != nullptr) {
            if (auto off = GetSourceOffsetRange(item.assignment->sourceRange, sm); off.has_value()) {
              rec.has_assignment_range = true;
              rec.assignment_start = off->first;
              rec.assignment_end = off->second;
            }
            rec.assignment_text = GetSourceText(item.assignment->sourceRange, sm);
            if (cache != nullptr) {
              rec.lhs_signals = MaterializeSignalPaths(GetCachedLhsSignals(item.assignment, *cache));
              rec.rhs_signals = MaterializeSignalPaths(GetCachedRhsSignals(item.assignment, *cache));
            } else {
              rec.lhs_signals = MaterializeSignalPaths(CollectLhsSignals(item.assignment));
              rec.rhs_signals = MaterializeSignalPaths(CollectRhsSignals(item.assignment));
            }
          } else if (!item.context_lhs_signals.empty()) {
            rec.lhs_signals = MaterializeSignalPaths(item.context_lhs_signals);
          }
          if (item.context_from_instance_port && item.context_instance != nullptr && item.context_port != nullptr) {
            const std::string port_signal = MakeInstancePortPath(item.context_instance, item.context_port);
            if (drivers_mode) {
              if (std::find(rec.rhs_signals.begin(), rec.rhs_signals.end(),
                            port_signal) == rec.rhs_signals.end()) {
                rec.rhs_signals.push_back(port_signal);
              }
            } else {
              if (std::find(rec.lhs_signals.begin(), rec.lhs_signals.end(),
                            port_signal) == rec.lhs_signals.end()) {
                rec.lhs_signals.push_back(port_signal);
              }
            }
          }
          std::sort(rec.lhs_signals.begin(), rec.lhs_signals.end());
          rec.lhs_signals.erase(std::unique(rec.lhs_signals.begin(), rec.lhs_signals.end()),
                                rec.lhs_signals.end());
          std::sort(rec.rhs_signals.begin(), rec.rhs_signals.end());
          rec.rhs_signals.erase(std::unique(rec.rhs_signals.begin(), rec.rhs_signals.end()),
                                rec.rhs_signals.end());
        }
      },
      r);
  return rec;
}

void CollectTraceableSymbols(const slang::ast::RootSymbol &root,
                             slang::flat_hash_map<std::string, const slang::ast::Symbol *> &out) {
  auto collect_from_scope = [&](const slang::ast::Scope &scope) {
    for (const auto &net : scope.membersOfType<slang::ast::NetSymbol>()) {
      out.try_emplace(std::string(net.getHierarchicalPath()), &net);
    }
    for (const auto &var : scope.membersOfType<slang::ast::VariableSymbol>()) {
      out.try_emplace(std::string(var.getHierarchicalPath()), &var);
    }
  };

  auto visit_structural = [&](auto &self, const slang::ast::Scope &scope) -> void {
    for (const auto &member : scope.members()) {
      if (member.kind == slang::ast::SymbolKind::Instance) {
        const auto &child_inst = member.as<slang::ast::InstanceSymbol>();
        collect_from_scope(child_inst.body);
        self(self, child_inst.body);
      } else if (member.kind == slang::ast::SymbolKind::InstanceArray) {
        for (const auto &elem_ptr : member.as<slang::ast::InstanceArraySymbol>().elements) {
          const auto &elem = elem_ptr->as<slang::ast::InstanceSymbol>();
          collect_from_scope(elem.body);
          self(self, elem.body);
        }
      } else if (member.kind == slang::ast::SymbolKind::GenerateBlock) {
        const auto &gen = member.as<slang::ast::GenerateBlockSymbol>();
        if (!gen.isUninstantiated) {
          collect_from_scope(gen);
          self(self, gen);
        }
      } else if (member.kind == slang::ast::SymbolKind::GenerateBlockArray) {
        for (const auto &elem_ptr : member.as<slang::ast::GenerateBlockArraySymbol>().entries) {
          const auto &elem = elem_ptr->as<slang::ast::GenerateBlockSymbol>();
          if (!elem.isUninstantiated) {
            collect_from_scope(elem);
            self(self, elem);
          }
        }
      }
    }
  };

  for (const slang::ast::InstanceSymbol *top : root.topInstances) {
    collect_from_scope(top->body);
    visit_structural(visit_structural, top->body);
  }
}

void CollectInstanceHierarchy(const slang::ast::RootSymbol &root, const slang::SourceManager &sm,
                              TraceDb &db) {
  struct HierInstanceInfo {
    std::string module;
    std::string source_file;
    uint32_t source_line = 0;
  };

  slang::flat_hash_map<std::string, HierInstanceInfo> modules;
  modules.reserve(2000000); // Pre-allocate to prevent rehash fragmentation
  auto note_instance = [&](const slang::ast::InstanceSymbol &inst) {
    HierInstanceInfo info;
    info.module = std::string(inst.getDefinition().name);
    const auto loc = inst.getDefinition().location;
    if (loc.valid()) {
      info.source_file = std::string(sm.getFileName(loc));
      info.source_line = sm.getLineNumber(loc);
    }
    modules.try_emplace(std::string(inst.getHierarchicalPath()), std::move(info));
  };

  auto visit_structural = [&](auto &self, const slang::ast::Scope &scope) -> void {
    for (const auto &member : scope.members()) {
      if (member.kind == slang::ast::SymbolKind::Instance) {
        const auto &child_inst = member.as<slang::ast::InstanceSymbol>();
        note_instance(child_inst);
        self(self, child_inst.body);
      } else if (member.kind == slang::ast::SymbolKind::InstanceArray) {
        for (const auto &elem_ptr : member.as<slang::ast::InstanceArraySymbol>().elements) {
          const auto &elem = elem_ptr->as<slang::ast::InstanceSymbol>();
          note_instance(elem);
          self(self, elem.body);
        }
      } else if (member.kind == slang::ast::SymbolKind::GenerateBlock) {
        const auto &gen = member.as<slang::ast::GenerateBlockSymbol>();
        if (!gen.isUninstantiated) self(self, gen);
      } else if (member.kind == slang::ast::SymbolKind::GenerateBlockArray) {
        for (const auto &elem_ptr : member.as<slang::ast::GenerateBlockArraySymbol>().entries) {
          const auto &elem = elem_ptr->as<slang::ast::GenerateBlockSymbol>();
          if (!elem.isUninstantiated) self(self, elem);
        }
      }
    }
  };

  for (const slang::ast::InstanceSymbol *top : root.topInstances) {
    note_instance(*top);
    visit_structural(visit_structural, top->body);
  }

  for (const auto &[path, info] : modules) {
    auto &node = db.hierarchy[path];
    node.module = info.module;
    node.source_file = info.source_file;
    node.source_line = info.source_line;
  }
  for (const auto &[path, _] : db.hierarchy) {
    std::string_view parent = ParentPath(path);
    if (parent.empty()) continue;
    auto parent_it = db.hierarchy.find(std::string(parent));
    if (parent_it == db.hierarchy.end()) continue;
    parent_it->second.children.push_back(path);
  }
  for (auto &[_, node] : db.hierarchy) {
    std::sort(node.children.begin(), node.children.end());
    node.children.erase(std::unique(node.children.begin(), node.children.end()), node.children.end());
  }
}

void BuildHierarchyFromSignals(TraceDb &db) {
  if (!db.hierarchy.empty()) return;
  for (const auto &[sig, _] : db.signals) {
    std::string_view cur = ParentPath(sig);
    while (!cur.empty()) {
      auto it = db.hierarchy.find(std::string(cur));
      if (it == db.hierarchy.end()) {
        db.hierarchy.emplace(std::string(cur), HierNodeRecord{});
      }
      cur = ParentPath(cur);
    }
  }
  for (const auto &[path, _] : db.hierarchy) {
    std::string_view parent = ParentPath(path);
    if (parent.empty()) continue;
    auto it = db.hierarchy.find(std::string(parent));
    if (it == db.hierarchy.end()) continue;
    it->second.children.push_back(path);
  }
  for (auto &[_, node] : db.hierarchy) {
    std::sort(node.children.begin(), node.children.end());
    node.children.erase(std::unique(node.children.begin(), node.children.end()), node.children.end());
  }
}

bool IsUnderHierarchyRoot(const std::string &signal, const std::string &root) {
  if (root.empty()) return true;
  if (signal == root) return true;
  if (signal.size() <= root.size()) return false;
  if (signal.rfind(root, 0) != 0) return false;
  return signal[root.size()] == '.';
}

slang::flat_hash_map<std::string_view, size_t> BuildSubtreeSignalCounts(
    const std::vector<std::string> &keys) {
  slang::flat_hash_map<std::string_view, size_t> counts;
  for (const std::string &sig : keys) {
    std::string_view inst = ParentPath(sig);
    while (!inst.empty()) {
      counts[inst] += 1;
      inst = ParentPath(inst);
    }
  }
  return counts;
}

std::vector<PartitionRecord> PlanHierarchyPartitions(
    const TraceDb &hier_db, const slang::flat_hash_map<std::string_view, size_t> &subtree_counts,
    size_t budget, CompileLogger *logger) {
  std::vector<std::string> roots;
  roots.reserve(hier_db.hierarchy.size());
  for (const auto &[path, _] : hier_db.hierarchy) {
    if (ParentPath(path).empty()) roots.push_back(path);
  }
  std::sort(roots.begin(), roots.end());

  std::vector<PartitionRecord> out;
  std::function<void(const std::string &, size_t)> split = [&](const std::string &node, size_t depth) {
    auto itc = subtree_counts.find(node);
    const size_t cnt = (itc == subtree_counts.end()) ? 0 : itc->second;
    if (cnt == 0) return;
    const auto hit = hier_db.hierarchy.find(node);
    std::vector<std::string> active_children;
    if (hit != hier_db.hierarchy.end()) {
      active_children.reserve(hit->second.children.size());
      for (const std::string &ch : hit->second.children) {
        auto ic = subtree_counts.find(ch);
        if (ic != subtree_counts.end() && ic->second > 0) active_children.push_back(ch);
      }
      std::sort(active_children.begin(), active_children.end());
    }

    if (cnt <= budget || active_children.empty()) {
      out.push_back(PartitionRecord{node, cnt, depth});
      return;
    }
    if (logger != nullptr) {
      logger->Log("partition split: root=" + node + " signals=" + std::to_string(cnt) +
                  " children=" + std::to_string(active_children.size()));
    }
    size_t child_covered = 0;
    for (const std::string &ch : active_children) {
      auto cit = subtree_counts.find(ch);
      if (cit != subtree_counts.end()) child_covered += cit->second;
      split(ch, depth + 1);
    }
    if (cnt > child_covered) {
      out.push_back(PartitionRecord{node, cnt - child_covered, depth});
    }
  };

  for (const std::string &r : roots)
    split(r, 0);
  std::sort(out.begin(), out.end(), [](const PartitionRecord &a, const PartitionRecord &b) {
    if (a.root != b.root) return a.root < b.root;
    return a.depth < b.depth;
  });
  return out;
}

std::vector<std::vector<size_t>> BucketSignalsByPartitions(
    const std::vector<std::string> &keys, const std::vector<PartitionRecord> &parts) {
  std::vector<std::vector<size_t>> buckets(parts.size());
  if (parts.empty()) {
    buckets.resize(1);
    for (size_t i = 0; i < keys.size(); ++i)
      buckets[0].push_back(i);
    return buckets;
  }

  std::vector<size_t> order(parts.size());
  for (size_t i = 0; i < parts.size(); ++i)
    order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    if (parts[a].root.size() != parts[b].root.size()) return parts[a].root.size() > parts[b].root.size();
    return parts[a].root < parts[b].root;
  });

  for (size_t i = 0; i < keys.size(); ++i) {
    const std::string &sig = keys[i];
    size_t chosen = parts.size();
    for (size_t idx : order) {
      if (IsUnderHierarchyRoot(sig, parts[idx].root)) {
        chosen = idx;
        break;
      }
    }
    if (chosen == parts.size()) {
      if (buckets.empty()) buckets.resize(1);
      buckets[0].push_back(i);
      continue;
    }
    buckets[chosen].push_back(i);
  }
  return buckets;
}

bool SaveGraphDb(const std::string &db_path, const std::vector<std::string> &keys,
                 const slang::flat_hash_map<std::string, const slang::ast::Symbol *> &symbols,
                 const slang::SourceManager &sm, const TraceDb &hier_db, size_t &signal_count,
                 bool low_mem, CompileLogger *logger) {
  using Clock = std::chrono::steady_clock;
  auto fmt_seconds = [](const Clock::time_point &start, const Clock::time_point &end) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(3)
       << std::chrono::duration<double>(end - start).count();
    return os.str();
  };

  const auto t_total_start = Clock::now();
  if (logger != nullptr) {
    logger->Log("save_graph_db: begin keys=" + std::to_string(keys.size()) +
                " hier_nodes=" + std::to_string(hier_db.hierarchy.size()));
  }

  GraphDb graph;
  slang::flat_hash_map<std::string, uint32_t> string_index;
  std::vector<std::pair<uint32_t, uint32_t>> load_refs_flat;
  std::vector<std::pair<uint32_t, uint32_t>> driver_refs_flat;
  std::vector<std::pair<uint32_t, uint32_t>> assignment_lhs_refs_flat;
  TraceDb compact_db;
  TraceCompileCache trace_cache;
  EndpointMergeGroups merge_groups;

  graph.endpoints.reserve(5000000);
  string_index.reserve(2000000);
  load_refs_flat.reserve(5000000);
  driver_refs_flat.reserve(5000000);
  assignment_lhs_refs_flat.reserve(5000000);

  auto intern = [&](const std::string &s) -> uint32_t {
    return InternString(s, graph.strings, string_index);
  };
  auto append_signal_refs = [&](const std::vector<std::string> &signals, uint32_t &begin, uint32_t &count) {
    begin = static_cast<uint32_t>(graph.signal_refs.size());
    count = static_cast<uint32_t>(signals.size());
    for (const std::string &sig : signals)
      graph.signal_refs.push_back(intern(sig));
  };
  auto append_endpoint = [&](const EndpointRecord &e) {
    GraphEndpointRecord ge;
    ge.path_str_id = intern(e.path);
    ge.file_str_id = intern(e.file);
    ge.direction_str_id = e.direction.empty() ? std::numeric_limits<uint32_t>::max() : intern(e.direction);
    ge.bit_map_str_id = e.bit_map.empty() ? std::numeric_limits<uint32_t>::max() : intern(e.bit_map);
    ge.line = static_cast<uint32_t>(std::max(e.line, 0));
    ge.assignment_start = e.assignment_start;
    ge.assignment_end = e.assignment_end;
    append_signal_refs(e.lhs_signals, ge.lhs_begin, ge.lhs_count);
    append_signal_refs(e.rhs_signals, ge.rhs_begin, ge.rhs_count);
    ge.kind = (e.kind == EndpointKind::kPort) ? 1u : 0u;
    ge.bit_map_approximate = e.bit_map_approximate ? 1u : 0u;
    ge.has_assignment_range = e.has_assignment_range ? 1u : 0u;
    graph.endpoints.push_back(ge);
  };
  auto build_signal_record = [&](const slang::ast::Symbol *sym) -> SignalRecord {
    return BuildSignalRecord(sym, sm, trace_cache);
  };

  graph.signals.resize(keys.size());
  for (size_t i = 0; i < keys.size(); ++i)
    graph.signals[i].name_str_id = intern(keys[i]);

  const auto t_build_start = Clock::now();
  signal_count = 0;
  const slang::ast::InstanceBodySymbol* current_cache_body = nullptr;
  for (size_t sig_id = 0; sig_id < keys.size(); ++sig_id) {
    const std::string &path = keys[sig_id];
    auto it = symbols.find(path);
    if (it == symbols.end() || !IsTraceable(it->second)) continue;

    const slang::ast::InstanceBodySymbol *body = GetContainingInstance(it->second);
    if (body != current_cache_body) {
      if (low_mem && trace_cache.body_trace_indexes.size() > 200) {
        trace_cache.body_trace_indexes.clear();
        trace_cache.assignment_lhs_signals.clear();
        trace_cache.assignment_rhs_signals.clear();
        trace_cache.statement_lhs_signals.clear();
      }
      current_cache_body = body;
    }

    SignalRecord rec = build_signal_record(it->second);
    if (ShouldCompactGlobalNet(path, rec.loads.size())) {
      GlobalNetRecord g;
      g.category = ClassifyGlobalNetCategory(path);
      g.sinks = ExtractCompactSinkPaths(path, rec.loads);
      if (!g.sinks.empty()) {
        for (const std::string &sink : g.sinks)
          compact_db.global_sink_to_source[sink] = path;
        compact_db.global_nets.emplace(path, std::move(g));
        rec.loads.clear();
      }
    }
    MergeEndpointBitRangesInPlace(rec.drivers, merge_groups);
    MergeEndpointBitRangesInPlace(rec.loads, merge_groups);

    GraphSignalRecord &gs = graph.signals[sig_id];
    gs.driver_begin = static_cast<uint32_t>(graph.endpoints.size());
    gs.driver_count = static_cast<uint32_t>(rec.drivers.size());
    for (const EndpointRecord &e : rec.drivers) {
      append_endpoint(e);
      if (!e.path.empty()) driver_refs_flat.push_back({intern(e.path), static_cast<uint32_t>(sig_id)});
    }
    gs.load_begin = static_cast<uint32_t>(graph.endpoints.size());
    gs.load_count = static_cast<uint32_t>(rec.loads.size());
    for (const EndpointRecord &e : rec.loads) {
      append_endpoint(e);
      if (!e.path.empty()) load_refs_flat.push_back({intern(e.path), static_cast<uint32_t>(sig_id)});
      const std::vector<std::string> lhs_paths =
          e.lhs_signals.empty() ? InferAssignmentLhsPathsFromText(e.path, e.assignment_text) : e.lhs_signals;
      for (const std::string &lhs : lhs_paths) {
        if (!lhs.empty()) assignment_lhs_refs_flat.push_back({intern(lhs), static_cast<uint32_t>(sig_id)});
      }
    }
    ++signal_count;
  }
  const auto t_build_end = Clock::now();
  if (logger != nullptr) {
    logger->Log("save_graph_db: build_graph done elapsed_s=" +
                fmt_seconds(t_build_start, t_build_end) +
                " strings=" + std::to_string(graph.strings.size()) +
                " endpoints=" + std::to_string(graph.endpoints.size()));
  }

  auto finalize_path_refs =
      [&](std::vector<std::pair<uint32_t, uint32_t>> &flat_src, std::vector<GraphPathRefRange> &ranges,
          std::vector<uint32_t> &flat) {
        if (flat_src.empty()) return;
        std::sort(flat_src.begin(), flat_src.end());
        flat_src.erase(std::unique(flat_src.begin(), flat_src.end()), flat_src.end());
        
        flat.reserve(flat_src.size());
        uint32_t current_path = flat_src[0].first;
        uint32_t current_begin = 0;
        
        for (size_t i = 0; i < flat_src.size(); ++i) {
          if (flat_src[i].first != current_path) {
            GraphPathRefRange range;
            range.path_str_id = current_path;
            range.begin = current_begin;
            range.count = static_cast<uint32_t>(flat.size() - current_begin);
            ranges.push_back(range);
            current_path = flat_src[i].first;
            current_begin = static_cast<uint32_t>(flat.size());
          }
          flat.push_back(flat_src[i].second);
        }
        GraphPathRefRange range;
        range.path_str_id = current_path;
        range.begin = current_begin;
        range.count = static_cast<uint32_t>(flat.size() - current_begin);
        ranges.push_back(range);
      };
  finalize_path_refs(load_refs_flat, graph.load_ref_ranges, graph.load_ref_signal_ids);
  finalize_path_refs(driver_refs_flat, graph.driver_ref_ranges, graph.driver_ref_signal_ids);
  finalize_path_refs(assignment_lhs_refs_flat, graph.assignment_lhs_ref_ranges,
                     graph.assignment_lhs_ref_signal_ids);

  std::vector<std::string> hier_paths;
  hier_paths.reserve(hier_db.hierarchy.size());
  for (const auto &[path, _] : hier_db.hierarchy)
    hier_paths.push_back(path);
  std::sort(hier_paths.begin(), hier_paths.end());
  graph.hierarchy.reserve(hier_paths.size());
  for (const std::string &path : hier_paths) {
    const auto it = hier_db.hierarchy.find(path);
    if (it == hier_db.hierarchy.end()) continue;
    GraphHierarchyRecord gh;
    gh.path_str_id = intern(path);
    gh.module_str_id =
        it->second.module.empty() ? std::numeric_limits<uint32_t>::max() : intern(it->second.module);
    gh.file_str_id =
        it->second.source_file.empty() ? std::numeric_limits<uint32_t>::max() : intern(it->second.source_file);
    gh.line = it->second.source_line;
    gh.child_begin = static_cast<uint32_t>(graph.hierarchy_children.size());
    gh.child_count = static_cast<uint32_t>(it->second.children.size());
    for (const std::string &child : it->second.children)
      graph.hierarchy_children.push_back(intern(child));
    graph.hierarchy.push_back(gh);
  }

  std::vector<std::string> global_sources;
  global_sources.reserve(compact_db.global_nets.size());
  for (const auto &[path, _] : compact_db.global_nets)
    global_sources.push_back(path);
  std::sort(global_sources.begin(), global_sources.end());
  graph.global_nets.reserve(global_sources.size());
  for (const std::string &source : global_sources) {
    const auto it = compact_db.global_nets.find(source);
    if (it == compact_db.global_nets.end()) continue;
    GraphGlobalNetRecord gg;
    gg.source_path_str_id = intern(source);
    gg.category_str_id = it->second.category.empty() ? std::numeric_limits<uint32_t>::max()
                                                     : intern(it->second.category);
    gg.sink_begin = static_cast<uint32_t>(graph.global_sinks.size());
    gg.sink_count = static_cast<uint32_t>(it->second.sinks.size());
    for (const std::string &sink : it->second.sinks)
      graph.global_sinks.push_back(intern(sink));
    graph.global_nets.push_back(gg);
  }

  std::vector<uint32_t> string_offsets;
  string_offsets.reserve(graph.strings.size() + 1);
  size_t total_str_bytes = 0;
  for (const std::string &s : graph.strings) total_str_bytes += s.size();
  std::string string_blob;
  string_blob.reserve(total_str_bytes);
  for (const std::string &s : graph.strings) {
    string_offsets.push_back(static_cast<uint32_t>(string_blob.size()));
    string_blob += s;
  }
  string_offsets.push_back(static_cast<uint32_t>(string_blob.size()));

  GraphDbFileHeader header;
  std::memcpy(header.magic, kGraphDbMagic, sizeof(header.magic));
  header.string_count = graph.strings.size();
  header.string_blob_size = string_blob.size();
  header.signal_count = graph.signals.size();
  header.endpoint_count = graph.endpoints.size();
  header.signal_ref_count = graph.signal_refs.size();
  header.load_ref_range_count = graph.load_ref_ranges.size();
  header.load_ref_count = graph.load_ref_signal_ids.size();
  header.driver_ref_range_count = graph.driver_ref_ranges.size();
  header.driver_ref_count = graph.driver_ref_signal_ids.size();
  header.assignment_lhs_ref_range_count = graph.assignment_lhs_ref_ranges.size();
  header.assignment_lhs_ref_count = graph.assignment_lhs_ref_signal_ids.size();
  header.hierarchy_count = graph.hierarchy.size();
  header.hierarchy_child_count = graph.hierarchy_children.size();
  header.global_net_count = graph.global_nets.size();
  header.global_sink_count = graph.global_sinks.size();

  const auto t_write_start = Clock::now();
  ScopedFileLock db_lock;
  if (!db_lock.Acquire(db_path)) return false;
  std::ofstream out(db_path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) return false;
  if (!WriteBinaryValue(out, header) || !WriteBinaryVector(out, string_offsets)) return false;
  out.write(string_blob.data(), static_cast<std::streamsize>(string_blob.size()));
  if (!out.good()) return false;
  if (!WriteBinaryVector(out, graph.signals) || !WriteBinaryVector(out, graph.endpoints) ||
      !WriteBinaryVector(out, graph.signal_refs) || !WriteBinaryVector(out, graph.load_ref_ranges) ||
      !WriteBinaryVector(out, graph.load_ref_signal_ids) || !WriteBinaryVector(out, graph.driver_ref_ranges) ||
      !WriteBinaryVector(out, graph.driver_ref_signal_ids) ||
      !WriteBinaryVector(out, graph.assignment_lhs_ref_ranges) ||
      !WriteBinaryVector(out, graph.assignment_lhs_ref_signal_ids) ||
      !WriteBinaryVector(out, graph.hierarchy) || !WriteBinaryVector(out, graph.hierarchy_children) ||
      !WriteBinaryVector(out, graph.global_nets) || !WriteBinaryVector(out, graph.global_sinks)) {
    return false;
  }
  const auto t_write_end = Clock::now();
  if (logger != nullptr) {
    logger->Log("save_graph_db: write_file done elapsed_s=" +
                fmt_seconds(t_write_start, t_write_end));
    logger->Log("save_graph_db: done elapsed_s=" +
                fmt_seconds(t_total_start, Clock::now()));
  }
  return true;
}

bool ValidateGraphRange(uint32_t begin, uint32_t count, size_t size) {
  return static_cast<uint64_t>(begin) + static_cast<uint64_t>(count) <=
         static_cast<uint64_t>(size);
}

bool ValidateGraphDb(const GraphDb &graph) {
  for (const GraphSignalRecord &gs : graph.signals) {
    if (!ValidateGraphRange(gs.driver_begin, gs.driver_count, graph.endpoints.size())) return false;
    if (!ValidateGraphRange(gs.load_begin, gs.load_count, graph.endpoints.size())) return false;
  }

  for (const GraphEndpointRecord &ge : graph.endpoints) {
    if (!ValidateGraphRange(ge.lhs_begin, ge.lhs_count, graph.signal_refs.size())) return false;
    if (!ValidateGraphRange(ge.rhs_begin, ge.rhs_count, graph.signal_refs.size())) return false;
  }

  auto validate_path_ref_ranges =
      [&](const std::vector<GraphPathRefRange> &ranges, const std::vector<uint32_t> &flat_refs) {
        for (const GraphPathRefRange &range : ranges) {
          if (!ValidateGraphRange(range.begin, range.count, flat_refs.size())) return false;
        }
        for (uint32_t sig_id : flat_refs) {
          if (sig_id >= graph.signals.size()) return false;
        }
        return true;
      };

  if (!validate_path_ref_ranges(graph.load_ref_ranges, graph.load_ref_signal_ids)) return false;
  if (!validate_path_ref_ranges(graph.driver_ref_ranges, graph.driver_ref_signal_ids)) return false;
  if (!validate_path_ref_ranges(graph.assignment_lhs_ref_ranges, graph.assignment_lhs_ref_signal_ids)) return false;

  for (const GraphHierarchyRecord &gh : graph.hierarchy) {
    if (!ValidateGraphRange(gh.child_begin, gh.child_count, graph.hierarchy_children.size())) return false;
  }

  for (const GraphGlobalNetRecord &gg : graph.global_nets) {
    if (!ValidateGraphRange(gg.sink_begin, gg.sink_count, graph.global_sinks.size())) return false;
  }

  return true;
}

bool LoadGraphDb(const std::string &db_path, GraphDb &graph, TraceDb &compat_db) {
  std::ifstream in(db_path, std::ios::binary);
  if (!in.is_open()) return false;

  GraphDbFileHeader header;
  if (!ReadBinaryValue(in, header)) return false;
  if (std::memcmp(header.magic, kGraphDbMagic, sizeof(header.magic)) != 0) return false;
  if (header.version != 1 && header.version != 2 && header.version != 3) return false;

  std::vector<uint32_t> string_offsets;
  if (!ReadBinaryVector(in, string_offsets, static_cast<size_t>(header.string_count + 1))) return false;
  std::string string_blob(header.string_blob_size, '\0');
  if (header.string_blob_size != 0) {
    in.read(string_blob.data(), static_cast<std::streamsize>(string_blob.size()));
    if (!in.good()) return false;
  }
  graph.strings.resize(static_cast<size_t>(header.string_count));
  for (size_t i = 0; i < graph.strings.size(); ++i) {
    const size_t start = string_offsets[i];
    const size_t end = string_offsets[i + 1];
    if (end < start || end > string_blob.size()) return false;
    graph.strings[i] = string_blob.substr(start, end - start);
  }

  if (!ReadBinaryVector(in, graph.signals, static_cast<size_t>(header.signal_count)) ||
      !ReadBinaryVector(in, graph.endpoints, static_cast<size_t>(header.endpoint_count)) ||
      !ReadBinaryVector(in, graph.signal_refs, static_cast<size_t>(header.signal_ref_count)) ||
      !ReadBinaryVector(in, graph.load_ref_ranges, static_cast<size_t>(header.load_ref_range_count)) ||
      !ReadBinaryVector(in, graph.load_ref_signal_ids, static_cast<size_t>(header.load_ref_count)) ||
      !ReadBinaryVector(in, graph.driver_ref_ranges, static_cast<size_t>(header.driver_ref_range_count)) ||
      !ReadBinaryVector(in, graph.driver_ref_signal_ids, static_cast<size_t>(header.driver_ref_count))) {
    return false;
  }
  if (header.version >= 2) {
    if (!ReadBinaryVector(in, graph.assignment_lhs_ref_ranges,
                          static_cast<size_t>(header.assignment_lhs_ref_range_count)) ||
        !ReadBinaryVector(in, graph.assignment_lhs_ref_signal_ids,
                          static_cast<size_t>(header.assignment_lhs_ref_count))) {
      return false;
    }
  }
  if (header.version >= 3) {
    if (!ReadBinaryVector(in, graph.hierarchy, static_cast<size_t>(header.hierarchy_count))) {
      return false;
    }
  } else {
    std::vector<GraphHierarchyRecordV2> hierarchy_v2;
    if (!ReadBinaryVector(in, hierarchy_v2, static_cast<size_t>(header.hierarchy_count))) {
      return false;
    }
    graph.hierarchy.reserve(hierarchy_v2.size());
    for (const GraphHierarchyRecordV2 &old_rec : hierarchy_v2) {
      GraphHierarchyRecord rec;
      rec.path_str_id = old_rec.path_str_id;
      rec.module_str_id = old_rec.module_str_id;
      rec.child_begin = old_rec.child_begin;
      rec.child_count = old_rec.child_count;
      graph.hierarchy.push_back(rec);
    }
  }
  if (!ReadBinaryVector(in, graph.hierarchy_children, static_cast<size_t>(header.hierarchy_child_count)) ||
      !ReadBinaryVector(in, graph.global_nets, static_cast<size_t>(header.global_net_count)) ||
      !ReadBinaryVector(in, graph.global_sinks, static_cast<size_t>(header.global_sink_count))) {
    return false;
  }
  if (!ValidateGraphDb(graph)) return false;

  compat_db.db_dir = std::filesystem::path(db_path).parent_path().string();
  compat_db.signals.clear();
  compat_db.hierarchy.clear();
  compat_db.global_nets.clear();
  compat_db.global_sink_to_source.clear();

  graph.load_ref_index.clear();
  graph.driver_ref_index.clear();
  graph.assignment_lhs_ref_index.clear();
  for (size_t i = 0; i < graph.load_ref_ranges.size(); ++i)
    graph.load_ref_index.emplace(graph.load_ref_ranges[i].path_str_id, i);
  for (size_t i = 0; i < graph.driver_ref_ranges.size(); ++i)
    graph.driver_ref_index.emplace(graph.driver_ref_ranges[i].path_str_id, i);
  for (size_t i = 0; i < graph.assignment_lhs_ref_ranges.size(); ++i)
    graph.assignment_lhs_ref_index.emplace(graph.assignment_lhs_ref_ranges[i].path_str_id, i);

  for (const GraphHierarchyRecord &gh : graph.hierarchy) {
    const std::string &path = GraphString(graph, gh.path_str_id);
    auto &node = compat_db.hierarchy[path];
    node.module = GraphString(graph, gh.module_str_id);
    if (header.version >= 3 && gh.file_str_id != std::numeric_limits<uint32_t>::max()) {
      node.source_file = GraphString(graph, gh.file_str_id);
      node.source_line = gh.line;
    }
    node.children.reserve(gh.child_count);
    for (uint32_t i = 0; i < gh.child_count; ++i)
      node.children.push_back(GraphString(graph, graph.hierarchy_children[gh.child_begin + i]));
  }
  for (const GraphGlobalNetRecord &gg : graph.global_nets) {
    const std::string &source = GraphString(graph, gg.source_path_str_id);
    auto &rec = compat_db.global_nets[source];
    rec.category = GraphString(graph, gg.category_str_id);
    rec.sinks.reserve(gg.sink_count);
    for (uint32_t i = 0; i < gg.sink_count; ++i) {
      const std::string &sink = GraphString(graph, graph.global_sinks[gg.sink_begin + i]);
      rec.sinks.push_back(sink);
      compat_db.global_sink_to_source[sink] = source;
    }
  }
  return true;
}

std::string StatMtimeString(const std::string &path) {
  std::error_code ec;
  const auto ts = std::filesystem::last_write_time(path, ec);
  if (ec) return "";
  return std::to_string(ts.time_since_epoch().count());
}

void BuildSessionSignalIndex(TraceSession &session) {
  if (session.signal_index_ready) return;
  session.signal_name_to_id.clear();
  session.signal_names_by_id.clear();
  GraphDb &graph = *session.graph;
  session.signal_name_to_id.reserve(graph.signals.size());
  session.signal_names_by_id.reserve(graph.signals.size());
  for (uint32_t i = 0; i < graph.signals.size(); ++i) {
    const std::string &name = GraphString(graph, graph.signals[i].name_str_id);
    session.signal_name_to_id.emplace(std::string_view(name), i);
    session.signal_names_by_id.push_back(&name);
  }
  session.signal_index_ready = true;
}

void EnsureSessionHierarchy(TraceSession &session) {
  if (session.hierarchy_ready) return;
  BuildHierarchyFromSignals(session.db);
  session.hierarchy_ready = true;
}

void BuildSessionReverseRefs(TraceSession &session) {
  if (session.reverse_refs_ready) return;
  session.reverse_refs_ready = true;
}

bool OpenTraceSession(const std::string &db_path, TraceSession &session, uint32_t flags) {
  TraceSession fresh;
  fresh.db_path = db_path;
  fresh.db_mtime = StatMtimeString(db_path);
  GraphDb graph;
  if (!LoadGraphDb(db_path, graph, fresh.db)) return false;
  fresh.graph = std::move(graph);
  session = std::move(fresh);
  BuildSessionSignalIndex(session);
  if (flags & kSessionHierarchy) EnsureSessionHierarchy(session);
  if (flags & kSessionReverseRefs) BuildSessionReverseRefs(session);
  return true;
}

std::optional<uint32_t> LookupSignalId(const TraceSession &session, std::string_view name) {
  auto it = session.signal_name_to_id.find(name);
  if (it == session.signal_name_to_id.end()) return std::nullopt;
  return it->second;
}

const std::string &SessionSignalName(const TraceSession &session, uint32_t id) {
  if (id >= session.signal_names_by_id.size()) throw std::out_of_range("invalid signal id");
  return *session.signal_names_by_id[id];
}

const SignalRecord &SessionSignalRecord(TraceSession &session, uint32_t id) {
  if (id >= session.graph->signals.size()) throw std::out_of_range("invalid signal id");
  auto cached = session.materialized_signal_records.find(id);
  if (cached != session.materialized_signal_records.end()) return cached->second;
  const GraphDb &graph = *session.graph;
  const GraphSignalRecord &gs = graph.signals[id];
  SignalRecord rec;
  auto materialize_endpoint = [&](const GraphEndpointRecord &ge) {
    EndpointRecord e;
    e.kind = (ge.kind == 1u) ? EndpointKind::kPort : EndpointKind::kExpr;
    e.path = GraphString(graph, ge.path_str_id);
    e.file = GraphString(graph, ge.file_str_id);
    e.path_id = ge.path_str_id;
    e.file_id = ge.file_str_id;
    e.line = static_cast<int>(ge.line);
    e.direction = GraphString(graph, ge.direction_str_id);
    e.bit_map = GraphString(graph, ge.bit_map_str_id);
    e.bit_map_approximate = (ge.bit_map_approximate != 0);
    e.has_assignment_range = (ge.has_assignment_range != 0);
    e.assignment_start = ge.assignment_start;
    e.assignment_end = ge.assignment_end;
    e.lhs_signals.reserve(ge.lhs_count);
    for (uint32_t i = 0; i < ge.lhs_count; ++i)
      e.lhs_signals.push_back(GraphString(graph, graph.signal_refs[ge.lhs_begin + i]));
    e.rhs_signals.reserve(ge.rhs_count);
    for (uint32_t i = 0; i < ge.rhs_count; ++i)
      e.rhs_signals.push_back(GraphString(graph, graph.signal_refs[ge.rhs_begin + i]));
    return e;
  };
  rec.drivers.reserve(gs.driver_count);
  for (uint32_t i = 0; i < gs.driver_count; ++i)
    rec.drivers.push_back(materialize_endpoint(graph.endpoints[gs.driver_begin + i]));
  rec.loads.reserve(gs.load_count);
  for (uint32_t i = 0; i < gs.load_count; ++i)
    rec.loads.push_back(materialize_endpoint(graph.endpoints[gs.load_begin + i]));
  return session.materialized_signal_records.emplace(id, std::move(rec)).first->second;
}

std::vector<uint32_t> SessionBridgeRefs(const TraceSession &session, bool use_load_refs,
                                        uint32_t path_id) {
  const GraphDb &graph = *session.graph;
  const auto &index = use_load_refs ? graph.load_ref_index : graph.driver_ref_index;
  const auto &ranges = use_load_refs ? graph.load_ref_ranges : graph.driver_ref_ranges;
  const auto &flat = use_load_refs ? graph.load_ref_signal_ids : graph.driver_ref_signal_ids;
  auto it = index.find(path_id);
  if (it == index.end()) return {};
  const GraphPathRefRange &range = ranges[it->second];
  if (!ValidateGraphRange(range.begin, range.count, flat.size())) {
    throw std::out_of_range("invalid path-ref range");
  }
  return std::vector<uint32_t>(flat.begin() + range.begin, flat.begin() + range.begin + range.count);
}

std::vector<uint32_t> SessionAssignmentLhsRefs(const TraceSession &session, uint32_t path_id) {
  const GraphDb &graph = *session.graph;
  const auto it = graph.assignment_lhs_ref_index.find(path_id);
  if (it == graph.assignment_lhs_ref_index.end()) return {};
  const GraphPathRefRange &range = graph.assignment_lhs_ref_ranges[it->second];
  const auto &flat = graph.assignment_lhs_ref_signal_ids;
  if (!ValidateGraphRange(range.begin, range.count, flat.size())) {
    throw std::out_of_range("invalid assignment-lhs range");
  }
  return std::vector<uint32_t>(flat.begin() + range.begin, flat.begin() + range.begin + range.count);
}

bool HasTimescaleArg(const std::vector<std::string> &args) {
  for (const std::string &arg : args) {
    if (arg == "--timescale") return true;
    if (arg.rfind("--timescale=", 0) == 0) return true;
  }
  return false;
}

bool HasUnknownSysNameWarningControl(const std::vector<std::string> &args) {
  for (const std::string &arg : args) {
    if (arg == "-Wunknown-sys-name" || arg == "-Wno-unknown-sys-name") return true;
    if (arg.rfind("-Wunknown-sys-name=", 0) == 0) return true;
    if (arg.rfind("-Wno-unknown-sys-name=", 0) == 0) return true;
  }
  return false;
}

bool IsDollarTokenDiagnostic(const slang::Diagnostic &diag, const slang::SourceManager &sm) {
  if (!diag.location.valid()) return false;
  const slang::SourceLocation loc = diag.location;
  const std::string_view text = sm.getSourceText(loc.buffer());
  if (loc.offset() >= text.size()) return false;
  return text[loc.offset()] == '$';
}

bool IsDefparamRelaxDiag(slang::DiagCode code) {
  const std::string_view name = slang::toString(code);
  if (name == "CouldNotResolveHierarchicalPath") return true;
  if (name.rfind("DefParam", 0) == 0 || name.rfind("Defparam", 0) == 0) return true;
  if (name == "VirtualIfaceDefparam") return true;
  return false;
}

bool IsDefparamContextDiagnostic(const slang::Diagnostic &diag, const slang::SourceManager &sm) {
  if (!diag.location.valid()) return false;
  const slang::SourceLocation loc = diag.location;
  const std::string_view text = sm.getSourceText(loc.buffer());
  if (loc.offset() >= text.size()) return false;
  size_t line_begin = text.rfind("\n", loc.offset());
  line_begin = (line_begin == std::string_view::npos) ? 0 : (line_begin + 1);
  size_t line_end = text.find("\n", loc.offset());
  if (line_end == std::string_view::npos) line_end = text.size();
  std::string line(text.substr(line_begin, line_end - line_begin));
  for (char &c : line) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return line.find("defparam") != std::string::npos;
}

bool IsIgnoredCompileDiag(const slang::Diagnostic &diag, const slang::SourceManager &sm,
                          bool relax_defparam) {
  const slang::DiagCode code = diag.code;
  const std::string_view name = slang::toString(code);
  // Any $systemcall / $systemfunc diagnostics should not block DB compile.
  if (code.getSubsystem() == slang::DiagSubsystem::SysFuncs) return true;
  if (IsDollarTokenDiagnostic(diag, sm)) return true;
  if (relax_defparam && (IsDefparamRelaxDiag(code) || IsDefparamContextDiagnostic(diag, sm))) return true;
  // Keep explicit compatibility for unknown vendor system names.
  return name == "UnknownSystemName";
}

bool HasBlockingCompileDiagnostics(slang::ast::Compilation &compilation, bool relax_defparam) {
  const slang::SourceManager &sm = *compilation.getSourceManager();
  for (const slang::Diagnostic &diag : compilation.getAllDiagnostics()) {
    const auto severity = slang::getDefaultSeverity(diag.code);
    if (severity != slang::DiagnosticSeverity::Error &&
        severity != slang::DiagnosticSeverity::Fatal) {
      continue;
    }
    if (IsIgnoredCompileDiag(diag, sm, relax_defparam)) continue;
    return true;
  }
  return false;
}

std::string ToLower(std::string s) {
  for (char &c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::optional<OutputFormat> ParseOutputFormat(const std::string &s) {
  const std::string lower = ToLower(s);
  if (lower == "text") return OutputFormat::kText;
  if (lower == "json") return OutputFormat::kJson;
  return std::nullopt;
}

bool ParseUnsignedCliValue(const std::string &flag, const std::string &value, size_t &out) {
  try {
    size_t pos = 0;
    const unsigned long long parsed = std::stoull(value, &pos, 10);
    if (pos != value.size()) {
      std::cerr << "Invalid " << flag << ": " << value << "\n";
      return false;
    }
    out = static_cast<size_t>(parsed);
    return true;
  } catch (...) {
    std::cerr << "Invalid " << flag << ": " << value << "\n";
    return false;
  }
}

std::string JsonEscape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
    case '\\': out += "\\\\"; break;
    case '"': out += "\\\""; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        out += "\\u00";
        const char *hex = "0123456789abcdef";
        out += hex[(c >> 4) & 0xf];
        out += hex[c & 0xf];
      } else {
        out += c;
      }
    }
  }
  return out;
}

bool RegexMatch(const std::optional<std::regex> &re, const std::string &s) {
  if (!re.has_value()) return true;
  return std::regex_search(s, *re);
}

bool ParseSignalQuery(const std::string &input, std::string &base_signal,
                      std::optional<std::pair<int32_t, int32_t>> &select) {
  static const std::regex kSelectRe(R"(^(.+)\[([0-9]+)(?::([0-9]+))?\]$)");
  std::smatch m;
  if (!std::regex_match(input, m, kSelectRe)) {
    base_signal = input;
    select.reset();
    return true;
  }
  if (m.size() != 4) return false;
  base_signal = m[1].str();
  try {
    const int32_t left = static_cast<int32_t>(std::stoll(m[2].str()));
    const int32_t right = m[3].matched ? static_cast<int32_t>(std::stoll(m[3].str())) : left;
    select = std::make_pair(left, right);
  } catch (...) { return false; }
  return !base_signal.empty();
}

std::optional<std::pair<int32_t, int32_t>> ParseExactBitRange(const EndpointRecord &e) {
  if (e.bit_map.empty() || e.bit_map.front() != '[') return std::nullopt;
  const size_t close = e.bit_map.find(']');
  if (close == std::string::npos || close <= 1) return std::nullopt;
  const std::string inside = e.bit_map.substr(1, close - 1);
  const size_t colon = inside.find(':');
  try {
    if (colon == std::string::npos) {
      const int32_t b = static_cast<int32_t>(std::stoll(inside));
      return std::make_pair(b, b);
    }
    const int32_t l = static_cast<int32_t>(std::stoll(inside.substr(0, colon)));
    const int32_t r = static_cast<int32_t>(std::stoll(inside.substr(colon + 1)));
    return std::make_pair(l, r);
  } catch (...) { return std::nullopt; }
}

bool RangesOverlap(const std::pair<int32_t, int32_t> &a, const std::pair<int32_t, int32_t> &b) {
  const int32_t alo = std::min(a.first, a.second);
  const int32_t ahi = std::max(a.first, a.second);
  const int32_t blo = std::min(b.first, b.second);
  const int32_t bhi = std::max(b.first, b.second);
  return std::max(alo, blo) <= std::min(ahi, bhi);
}

bool EndpointMatchesSignalSelect(const EndpointRecord &e,
                                 const std::optional<std::pair<int32_t, int32_t>> &select) {
  if (!select.has_value()) return true;
  if (e.kind == EndpointKind::kPort) return true;
  if (e.bit_map.empty() || e.bit_map_approximate) return true;
  const std::optional<std::pair<int32_t, int32_t>> endpoint_range = ParseExactBitRange(e);
  if (!endpoint_range.has_value()) return true;
  return RangesOverlap(*endpoint_range, *select);
}

std::string EndpointKey(const TraceDb &db, const EndpointRecord &e) {
  return std::to_string(static_cast<int>(e.kind)) + "\t" + EndpointPath(db, e) + "\t" +
         EndpointFile(db, e) + "\t" + std::to_string(e.line) + "\t" + e.direction + "\t" +
         (e.has_assignment_range ? "1" : "0") + "\t" + std::to_string(e.assignment_start) + "\t" +
         std::to_string(e.assignment_end) + "\t" + e.assignment_text + "\t" + e.bit_map + "\t" +
         (e.bit_map_approximate ? "1" : "0");
}

size_t EditDistance(const std::string &a, const std::string &b) {
  std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
  for (size_t j = 0; j <= b.size(); ++j)
    prev[j] = j;
  for (size_t i = 1; i <= a.size(); ++i) {
    cur[0] = i;
    for (size_t j = 1; j <= b.size(); ++j) {
      const size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
      cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
    }
    std::swap(prev, cur);
  }
  return prev[b.size()];
}

bool LooksLikeOptionToken(const std::string &s) {
  return !s.empty() && (s[0] == '-' || s[0] == '+');
}

void CollectFilesFromFlist(const std::filesystem::path &flist,
                           std::vector<std::filesystem::path> &out) {
  std::ifstream in(flist);
  if (!in.is_open()) return;
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())))
      line.erase(line.begin());
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
      line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    if (LooksLikeOptionToken(line)) continue;
    std::filesystem::path p = line;
    if (p.is_relative()) p = flist.parent_path() / p;
    out.push_back(std::filesystem::weakly_canonical(p));
  }
}



bool HasTopArg(const std::vector<std::string> &args) {
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string &arg = args[i];
    if (arg == "--top" && i + 1 < args.size()) return true;
    if (arg.rfind("--top=", 0) == 0 && arg.size() > std::string("--top=").size()) return true;
  }
  return false;
}

std::string ToAbsPathString(const std::filesystem::path &p, const std::filesystem::path &base) {
  std::filesystem::path abs = p;
  if (abs.is_relative()) abs = base / abs;
  std::error_code ec;
  if (std::filesystem::exists(abs, ec)) {
    auto canon = std::filesystem::weakly_canonical(abs, ec);
    if (!ec) return canon.string();
  }
  return abs.lexically_normal().string();
}

bool ParsePlusList(std::string_view tok, std::string_view prefix, std::vector<std::string> &out,
                   const std::filesystem::path &base) {
  if (tok.rfind(prefix, 0) != 0) return false;
  std::string payload(tok.substr(prefix.size()));
  std::stringstream ss(payload);
  std::string part;
  while (std::getline(ss, part, '+')) {
    if (part.empty()) continue;
    out.push_back(ToAbsPathString(std::filesystem::path(part), base));
  }
  return true;
}

bool ParseDefinesPlus(std::string_view tok, std::vector<std::string> &out) {
  static constexpr std::string_view kPrefix = "+define+";
  if (tok.rfind(kPrefix, 0) != 0) return false;
  std::string payload(tok.substr(kPrefix.size()));
  std::stringstream ss(payload);
  std::string part;
  while (std::getline(ss, part, '+')) {
    if (!part.empty()) out.push_back(part);
  }
  return true;
}


std::string ComputeCompileFingerprint(const std::vector<std::string> &passthrough_args) {
  std::vector<std::string> parts;
  parts.push_back("rtl_trace_compile_fingerprint_v1");
  for (const std::string &arg : passthrough_args)
    parts.push_back("ARG:" + arg);

  std::vector<std::filesystem::path> files;
  for (size_t i = 0; i < passthrough_args.size(); ++i) {
    const std::string &arg = passthrough_args[i];
    if ((arg == "-f" || arg == "-F") && i + 1 < passthrough_args.size()) {
      std::filesystem::path flist = passthrough_args[++i];
      if (std::filesystem::exists(flist)) {
        flist = std::filesystem::weakly_canonical(flist);
        files.push_back(flist);
        CollectFilesFromFlist(flist, files);
      }
      continue;
    }
    if (LooksLikeOptionToken(arg)) continue;
    std::filesystem::path p = arg;
    if (std::filesystem::exists(p)) files.push_back(std::filesystem::weakly_canonical(p));
  }

  std::sort(files.begin(), files.end());
  files.erase(std::unique(files.begin(), files.end()), files.end());
  for (const auto &p : files) {
    std::error_code ec;
    const auto ts = std::filesystem::last_write_time(p, ec);
    if (ec) continue;
    const auto cnt = ts.time_since_epoch().count();
    parts.push_back("FILE:" + p.string() + ":" + std::to_string(cnt));
  }
  std::ostringstream os;
  for (const std::string &part : parts)
    os << part << '\n';
  return os.str();
}

void PrintGeneralHelp() {
  std::cout << "rtl_trace: standalone RTL driver/load tracer\n\n";
  std::cout << "Usage:\n";
  std::cout << "  rtl_trace compile [--db <file>] [--incremental] [--relax-defparam] [--mfcu] "
               "[--partition-budget <N>] [--compile-log <file>] [slang source args...]\n";
  std::cout << "  rtl_trace trace --db <file> --mode <drivers|loads> --signal <hier.path> "
               "[--cone-level <N>] "
               "[--prefer-port-hop] "
               "[--depth <N>] [--max-nodes <N>] [--include <regex>] [--exclude <regex>] "
               "[--stop-at <regex>] [--format <text|json>]\n";
  std::cout << "  rtl_trace hier --db <file> [--root <hier.path>] [--depth <N>] "
               "[--max-nodes <N>] [--format <text|json>] [--show-source]\n";
  std::cout << "  rtl_trace whereis-instance --db <file> --instance <hier.path> "
               "[--format <text|json>]\n";
  std::cout << "  rtl_trace find --db <file> --query <text|regex> [--regex] [--limit <N>] "
               "[--format <text|json>]\n";
  std::cout << "  rtl_trace serve [--db <file>]\n";
}

void PrintTraceHelp() {
  std::cout << "Usage: rtl_trace trace --db <file> --mode <drivers|loads> --signal "
               "<hier.path|hier.path[bit]|hier.path[msb:lsb]> "
               "[--cone-level <N>] [--prefer-port-hop] [--depth <N>] [--max-nodes <N>] "
               "[--include <regex>] [--exclude <regex>] [--stop-at <regex>] "
               "[--format <text|json>]\n";
}

void PrintHierHelp() {
  std::cout << "Usage: rtl_trace hier --db <file> [--root <hier.path>] [--depth <N>] "
               "[--max-nodes <N>] [--format <text|json>] [--show-source]\n";
}

void PrintWhereInstanceHelp() {
  std::cout << "Usage: rtl_trace whereis-instance --db <file> --instance <hier.path> "
               "[--format <text|json>]\n";
}

void PrintFindHelp() {
  std::cout << "Usage: rtl_trace find --db <file> --query <text|regex> [--regex] [--limit <N>] "
               "[--format <text|json>]\n";
}

void PrintServeHelp() {
  std::cout << "Usage: rtl_trace serve [--db <file>]\n\n";
  std::cout << "Interactive commands:\n";
  std::cout << "  status\n";
  std::cout << "  open --db <file>\n";
  std::cout << "  reload\n";
  std::cout << "  close\n";
  std::cout << "  find --query <text> [--regex] [--limit <N>] [--format <text|json>]\n";
  std::cout << "  trace --mode <drivers|loads> --signal <hier.path> [trace options]\n";
  std::cout << "  hier [--root <hier.path>] [--depth <N>] [--max-nodes <N>] [--format <text|json>] [--show-source]\n";
  std::cout << "  whereis-instance --instance <hier.path> [--format <text|json>]\n";
  std::cout << "  quit\n";
  std::cout << "\nEach response is followed by a line containing <<END>>.\n";
}


std::vector<std::string> ArgvToVector(int argc, char *argv[]) {
  std::vector<std::string> out;
  out.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i)
    out.emplace_back(argv[i]);
  return out;
}


std::vector<std::string> TopSuggestions(const TraceSession &session, const std::string &needle,
                                        size_t limit) {
  std::vector<std::pair<size_t, std::string>> scored;
  scored.reserve(session.signal_names_by_id.size());
  for (const std::string *name : session.signal_names_by_id) {
    scored.push_back({EditDistance(*name, needle), *name});
  }
  std::sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) {
    if (a.first != b.first) return a.first < b.first;
    return a.second < b.second;
  });
  std::vector<std::string> out;
  for (const auto &p : scored) {
    out.push_back(p.second);
    if (out.size() >= limit) break;
  }
  return out;
}

std::string ResolveSourcePath(const TraceDb &db, const std::string &file) {
  std::filesystem::path p(file);
  if (p.is_absolute()) return p.string();
  if (!db.db_dir.empty()) {
    const std::filesystem::path joined = std::filesystem::path(db.db_dir) / p;
    std::error_code ec;
    if (std::filesystem::exists(joined, ec)) return joined.string();
  }
  return p.string();
}

std::string FetchSourceSlice(TraceSession &session, const EndpointRecord &e) {
  if (!e.has_assignment_range || e.assignment_end <= e.assignment_start) return "";
  const std::string resolved = ResolveSourcePath(session.db, EndpointFile(session.db, e));
  auto it = session.source_file_cache.find(resolved);
  if (it == session.source_file_cache.end()) {
    std::ifstream in(resolved);
    if (!in.is_open()) return "";
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    it = session.source_file_cache.emplace(resolved, std::move(data)).first;
  }
  const std::string &text = it->second;
  const size_t start = static_cast<size_t>(e.assignment_start);
  const size_t end = static_cast<size_t>(e.assignment_end);
  if (start >= text.size() || end > text.size() || end <= start) return "";
  return text.substr(start, end - start);
}

std::string ResolveHierarchySourcePath(const TraceDb &db, const HierNodeRecord &node) {
  if (node.source_file.empty()) return "";
  return ResolveSourcePath(db, node.source_file);
}

std::vector<std::string> InferAssignmentLhsPaths(TraceSession &session, EndpointRecord &e) {
  if (!e.lhs_signals.empty()) return e.lhs_signals;
  if (e.kind != EndpointKind::kExpr) return {};
  if (e.assignment_text.empty()) e.assignment_text = FetchSourceSlice(session, e);
  if (e.assignment_text.empty()) return {};
  return InferAssignmentLhsPathsFromText(e.path, e.assignment_text);
}

void MaterializeAssignmentTexts(TraceSession &session, TraceRunResult &result) {
  for (EndpointRecord &e : result.endpoints) {
    if (e.kind != EndpointKind::kExpr) continue;
    if (!e.assignment_text.empty()) continue;
    if (!e.has_assignment_range) continue;
    e.assignment_text = FetchSourceSlice(session, e);
  }
}

void PrintTraceText(const TraceDb &db, const TraceOptions &opts, const TraceRunResult &result) {
  std::cout << "target: " << opts.signal << "\n";
  std::cout << "mode: " << opts.mode << "\n";
  std::cout << "cone_level: " << opts.cone_level << "\n";
  std::cout << "count: " << result.endpoints.size() << "\n";
  std::cout << "visited: " << result.visited_count << "\n";
  for (const EndpointRecord &e : result.endpoints) {
    const std::string &path = EndpointPath(db, e);
    const std::string &file = EndpointFile(db, e);
    if (e.kind == EndpointKind::kPort) {
      std::cout << "port  " << path << " (" << e.direction << ") @ " << file << ":" << e.line << "\n";
    } else {
      std::cout << "expr  " << path << " @ " << file << ":" << e.line << "\n";
      if (!e.bit_map.empty()) {
        std::cout << "  bits   " << e.bit_map;
        if (e.bit_map_approximate) std::cout << " (approx)";
        std::cout << "\n";
      }
      if (opts.mode == "drivers" && !e.assignment_text.empty()) {
        std::cout << "  assign " << e.assignment_text << "\n";
        for (const std::string &rhs : e.rhs_signals) {
          std::cout << "  rhs    " << rhs << "\n";
        }
      }
      if (opts.mode == "loads" && !e.assignment_text.empty()) {
        std::cout << "  assign " << e.assignment_text << "\n";
      }
      if (opts.mode == "loads" && !e.lhs_signals.empty()) {
        for (const std::string &lhs : e.lhs_signals) {
          std::cout << "  lhs    " << lhs << "\n";
        }
      }
    }
  }
  if (!result.stops.empty()) {
    std::cout << "stops: " << result.stops.size() << "\n";
    for (const TraceStop &s : result.stops) {
      std::cout << "  stop  " << s.reason << " signal=" << s.signal << " depth=" << s.depth;
      if (!s.detail.empty()) std::cout << " detail=" << s.detail;
      std::cout << "\n";
    }
  }
}

void PrintTraceJson(const TraceDb &db, const TraceOptions &opts, const TraceRunResult &result) {
  std::cout << "{";
  std::cout << "\"target\":\"" << JsonEscape(opts.signal) << "\",";
  std::cout << "\"mode\":\"" << JsonEscape(opts.mode) << "\",";
  std::cout << "\"summary\":{\"cone_level\":" << opts.cone_level << ",\"count\":"
            << result.endpoints.size() << ",\"visited\":"
            << result.visited_count << ",\"stops\":" << result.stops.size() << "},";
  std::cout << "\"endpoints\":[";
  for (size_t i = 0; i < result.endpoints.size(); ++i) {
    const EndpointRecord &e = result.endpoints[i];
    const std::string &path = EndpointPath(db, e);
    const std::string &file = EndpointFile(db, e);
    if (i) std::cout << ",";
    std::cout << "{"
              << "\"kind\":\"" << (e.kind == EndpointKind::kPort ? "port" : "expr") << "\","
              << "\"path\":\"" << JsonEscape(path) << "\","
              << "\"file\":\"" << JsonEscape(file) << "\","
              << "\"line\":" << e.line << ","
              << "\"direction\":\"" << JsonEscape(e.direction) << "\","
              << "\"bit_map\":\"" << JsonEscape(e.bit_map) << "\","
              << "\"bit_map_approximate\":" << (e.bit_map_approximate ? "true" : "false") << ","
              << "\"assignment\":\"" << JsonEscape(e.assignment_text) << "\","
              << "\"lhs\":[";
    for (size_t j = 0; j < e.lhs_signals.size(); ++j) {
      if (j) std::cout << ",";
      std::cout << "\"" << JsonEscape(e.lhs_signals[j]) << "\"";
    }
    std::cout << "],\"rhs\":[";
    for (size_t j = 0; j < e.rhs_signals.size(); ++j) {
      if (j) std::cout << ",";
      std::cout << "\"" << JsonEscape(e.rhs_signals[j]) << "\"";
    }
    std::cout << "]}";
  }
  std::cout << "],\"stops\":[";
  for (size_t i = 0; i < result.stops.size(); ++i) {
    const TraceStop &s = result.stops[i];
    if (i) std::cout << ",";
    std::cout << "{\"signal\":\"" << JsonEscape(s.signal) << "\",\"reason\":\"" << JsonEscape(s.reason)
              << "\",\"detail\":\"" << JsonEscape(s.detail) << "\",\"depth\":" << s.depth << "}";
  }
  std::cout << "]}";
  std::cout << "\n";
}


} // namespace rtl_trace
