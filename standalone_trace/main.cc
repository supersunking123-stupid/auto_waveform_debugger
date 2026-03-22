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
#include <filesystem>
#include <fstream>
#include <functional>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace {

struct ExprTraceResult {
  const slang::ast::NamedValueExpression *expr = nullptr;
  const slang::ast::AssignmentExpression *assignment = nullptr;
  std::vector<const slang::ast::Expression *> selectors;
  std::vector<std::string> context_lhs_signals;
  std::string context_instance_port_signal;
  bool context_from_instance_port = false;
  const slang::ast::InstanceSymbol *context_instance = nullptr;
  const slang::ast::PortSymbol *context_port = nullptr;
};

using TraceResult = std::variant<const slang::ast::PortSymbol *, ExprTraceResult>;

enum class EndpointKind { kPort, kExpr };

struct EndpointRecord {
  EndpointKind kind = EndpointKind::kExpr;
  std::string path;
  std::string file;
  uint32_t path_id = std::numeric_limits<uint32_t>::max();
  uint32_t file_id = std::numeric_limits<uint32_t>::max();
  int line = 0;
  std::string direction;
  std::string assignment_text;
  bool has_assignment_range = false;
  uint32_t assignment_start = 0;
  uint32_t assignment_end = 0;
  std::string bit_map;
  bool bit_map_approximate = false;
  std::vector<std::string> lhs_signals;
  std::vector<std::string> rhs_signals;
};

struct SignalRecord {
  std::vector<EndpointRecord> drivers;
  std::vector<EndpointRecord> loads;
};

struct HierNodeRecord {
  std::string module;
  std::vector<std::string> children;
};

struct GlobalNetRecord {
  std::string category;
  std::vector<std::string> sinks;
};

struct TraceDb {
  std::unordered_map<std::string, SignalRecord> signals;
  std::unordered_map<std::string, HierNodeRecord> hierarchy;
  std::unordered_map<std::string, GlobalNetRecord> global_nets;
  std::unordered_map<std::string, std::string> global_sink_to_source;
  std::vector<std::string> path_pool;
  std::vector<std::string> file_pool;
  std::string db_dir;
};

struct PartitionRecord {
  std::string root;
  size_t signal_count = 0;
  size_t depth = 0;
};

struct BodyTraceIndex {
  std::unordered_map<const slang::ast::Symbol *, std::vector<TraceResult>> drivers;
  std::unordered_map<const slang::ast::Symbol *, std::vector<TraceResult>> loads;
};

struct TraceCompileCache {
  std::unordered_map<const slang::ast::AssignmentExpression *, std::vector<std::string>>
      assignment_lhs_signals;
  std::unordered_map<const slang::ast::AssignmentExpression *, std::vector<std::string>>
      assignment_rhs_signals;
  std::unordered_map<const slang::ast::Statement *, std::vector<std::string>> statement_lhs_signals;
  std::unordered_map<const slang::ast::InstanceBodySymbol *, BodyTraceIndex> body_trace_indexes;
  std::unordered_map<const slang::ast::Symbol *, SignalRecord> signal_records;
};

class CompileLogger {
 public:
  explicit CompileLogger(const std::string &log_path) {
    if (!log_path.empty()) file_.open(log_path, std::ios::out | std::ios::trunc);
  }

  void Log(const std::string &msg) {
    const std::string line = "[rtl_trace] " + Timestamp() + " " + msg;
    std::cerr << line << "\n";
    if (file_.is_open()) {
      file_ << line << "\n";
      file_.flush();
    }
  }

 private:
  static std::string Timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    std::ostringstream os;
    os << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
    return os.str();
  }

  std::ofstream file_;
};

enum class OutputFormat { kText, kJson };

struct TraceOptions {
  std::string mode;
  std::string signal;
  std::string root_signal;
  std::optional<std::pair<int32_t, int32_t>> signal_select;
  size_t cone_level = 1;
  bool prefer_port_hop = false;
  size_t depth_limit = 8;
  size_t max_nodes = 5000;
  std::optional<std::regex> include_re;
  std::optional<std::regex> exclude_re;
  std::optional<std::regex> stop_at_re;
  OutputFormat format = OutputFormat::kText;
};

struct TraceStop {
  std::string signal;
  std::string reason;
  std::string detail;
  size_t depth = 0;
};

struct TraceRunResult {
  std::vector<EndpointRecord> endpoints;
  std::vector<TraceStop> stops;
  size_t visited_count = 0;
};

struct HierOptions {
  std::string root;
  size_t depth_limit = 8;
  size_t max_nodes = 5000;
  OutputFormat format = OutputFormat::kText;
};

struct HierTreeNode {
  std::string path;
  std::string module;
  std::vector<HierTreeNode> children;
};

struct HierRunResult {
  std::string root;
  size_t depth_limit = 0;
  size_t node_count = 0;
  bool truncated = false;
  std::vector<std::string> stops;
  std::optional<HierTreeNode> tree;
};

std::vector<std::string> CollectLhsSignalsFromStatement(const slang::ast::Statement &stmt);
const std::vector<std::string> &GetCachedLhsSignals(
    const slang::ast::AssignmentExpression *assignment, TraceCompileCache &cache);
const std::vector<std::string> &GetCachedRhsSignals(
    const slang::ast::AssignmentExpression *assignment, TraceCompileCache &cache);
const std::vector<std::string> &GetCachedStatementLhsSignals(
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

template <bool DRIVERS>
class BodyTraceIndexBuilder : public slang::ast::ASTVisitor<BodyTraceIndexBuilder<DRIVERS>, true, true> {
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
    std::vector<std::string> context_lhs = GetCachedStatementLhsSignals(stmt.ifTrue, cache_);
    if (stmt.ifFalse != nullptr) {
      const auto &else_lhs = GetCachedStatementLhsSignals(*stmt.ifFalse, cache_);
      context_lhs.insert(context_lhs.end(), else_lhs.begin(), else_lhs.end());
      std::sort(context_lhs.begin(), context_lhs.end());
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
    std::vector<std::string> context_lhs;
    for (const auto &item : stmt.items) {
      const auto &item_lhs = GetCachedStatementLhsSignals(*item.stmt, cache_);
      context_lhs.insert(context_lhs.end(), item_lhs.begin(), item_lhs.end());
    }
    if (stmt.defaultCase != nullptr) {
      const auto &default_lhs = GetCachedStatementLhsSignals(*stmt.defaultCase, cache_);
      context_lhs.insert(context_lhs.end(), default_lhs.begin(), default_lhs.end());
    }
    std::sort(context_lhs.begin(), context_lhs.end());
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
        if (checking_lhs_) result.assignment = current_assignment_;
        result.selectors = selector_stack_;
        if (checking_instance_port_expression_ && active_instance_ != nullptr && active_port_ != nullptr) {
          result.context_from_instance_port = true;
          result.context_instance = active_instance_;
          result.context_port = active_port_;
          result.context_instance_port_signal =
              std::string(active_instance_->getHierarchicalPath()) + "." + std::string(active_port_->name);
        }
        Entries()[&nve.symbol].push_back(std::move(result));
      }
    } else {
      if (!(checking_lhs_ && selector_depth_ == 0)) {
        ExprTraceResult result;
        result.expr = &nve;
        result.assignment = current_assignment_;
        result.selectors = selector_stack_;
        if (checking_instance_port_expression_ && active_instance_ != nullptr && active_port_ != nullptr) {
          result.context_from_instance_port = true;
          result.context_instance = active_instance_;
          result.context_port = active_port_;
          result.context_instance_port_signal =
              std::string(active_instance_->getHierarchicalPath()) + "." + std::string(active_port_->name);
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

  void handle(const slang::ast::UninstantiatedDefSymbol &uninst) {}

 private:
  std::unordered_map<const slang::ast::Symbol *, std::vector<TraceResult>> &Entries() {
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
  std::vector<std::vector<std::string>> condition_lhs_stack_;
  std::vector<std::vector<std::string>> timed_lhs_stack_;
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
    : public slang::ast::ASTVisitor<PortConnectionResultCollector, true, true> {
 public:
  PortConnectionResultCollector(std::vector<TraceResult> &out,
                                std::unordered_set<const slang::ast::Symbol *> &visited)
      : out_(out), visited_(visited) {}

  void handle(const slang::ast::NamedValueExpression &nve) {
    if (!IsTraceable(&nve.symbol)) return;
    if (!visited_.insert(&nve.symbol).second) return;
    ExprTraceResult result;
    result.expr = &nve;
    result.assignment = nullptr;
    result.context_from_instance_port = true;
    result.context_instance = instance_;
    result.context_port = port_;
    if (instance_ != nullptr && port_ != nullptr) {
      result.context_instance_port_signal =
          std::string(instance_->getHierarchicalPath()) + "." + std::string(port_->name);
    }
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
  const auto &entries = [&]() -> const std::unordered_map<const slang::ast::Symbol *, std::vector<TraceResult>> & {
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

const SignalRecord &GetOrBuildSignalRecord(const slang::ast::Symbol *sym, const slang::SourceManager &sm,
                                           TraceCompileCache &cache) {
  auto it = cache.signal_records.find(sym);
  if (it != cache.signal_records.end()) return it->second;

  SignalRecord rec;
  std::unordered_set<const slang::ast::Symbol *> visited_drivers;
  visited_drivers.insert(sym);
  for (const TraceResult &r : ComputeIndexedTraceResults</*DRIVERS*/ true>(sym, cache, visited_drivers))
    rec.drivers.push_back(ResolveTraceResult(r, sm, true, &cache));

  std::unordered_set<const slang::ast::Symbol *> visited_loads;
  visited_loads.insert(sym);
  for (const TraceResult &r : ComputeIndexedTraceResults</*DRIVERS*/ false>(sym, cache, visited_loads))
    rec.loads.push_back(ResolveTraceResult(r, sm, false, &cache));

  return cache.signal_records.emplace(sym, std::move(rec)).first->second;
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

std::string ParentPath(const std::string &path) {
  const size_t pos = path.rfind('.');
  if (pos == std::string::npos) return "";
  return path.substr(0, pos);
}

std::string LeafName(const std::string &path) {
  const size_t pos = path.rfind('.');
  if (pos == std::string::npos) return path;
  return path.substr(pos + 1);
}

std::pair<std::string, std::string> SplitPathPrefixLeaf(const std::string &path) {
  const size_t pos = path.rfind('.');
  if (pos == std::string::npos) return {"", path};
  return {path.substr(0, pos), path.substr(pos + 1)};
}

uint64_t Fnv1a64(std::string_view s) {
  uint64_t h = 1469598103934665603ull;
  for (char c : s) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ull;
  }
  return h;
}

uint32_t InternString(const std::string &s, std::vector<std::string> &pool,
                      std::unordered_map<std::string, uint32_t> &index) {
  auto it = index.find(s);
  if (it != index.end()) return it->second;
  const uint32_t id = static_cast<uint32_t>(pool.size());
  pool.push_back(s);
  index.emplace(pool.back(), id);
  return id;
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

std::string JoinLines(const std::vector<std::string> &items) {
  std::string joined;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i != 0) joined += '\n';
    joined += items[i];
  }
  return joined;
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

std::string EndpointMergeKey(const EndpointRecord &e) {
  std::string lhs;
  for (size_t i = 0; i < e.lhs_signals.size(); ++i) {
    if (i) lhs += '\n';
    lhs += e.lhs_signals[i];
  }
  std::string rhs;
  for (size_t i = 0; i < e.rhs_signals.size(); ++i) {
    if (i) rhs += '\n';
    rhs += e.rhs_signals[i];
  }
  return std::to_string(static_cast<int>(e.kind)) + "\t" + e.path + "\t" + e.file + "\t" +
         std::to_string(e.line) + "\t" + e.direction + "\t" + (e.has_assignment_range ? "1" : "0") + "\t" +
         std::to_string(e.assignment_start) + "\t" + std::to_string(e.assignment_end) + "\t" +
         e.assignment_text + "\t" + lhs + "\t" + rhs;
}

std::vector<EndpointRecord> MergeEndpointBitRanges(std::vector<EndpointRecord> endpoints) {
  std::vector<EndpointRecord> out;
  std::unordered_map<std::string, std::vector<std::pair<std::pair<int32_t, int32_t>, EndpointRecord>>> groups;
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
    groups[EndpointMergeKey(e)].push_back({{lo, hi}, std::move(e)});
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
  return out;
}

constexpr size_t kCompactGlobalNetThreshold = 1024;

bool LooksLikeClockOrResetName(std::string_view path) {
  std::string leaf = LeafName(std::string(path));
  for (char &c : leaf)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (leaf == "clk" || leaf == "clock" || leaf == "rst" || leaf == "reset" || leaf == "rst_n" ||
      leaf == "reset_n" || leaf == "resetn" || leaf == "rstn")
    return true;
  return leaf.find("clk") != std::string::npos || leaf.find("clock") != std::string::npos ||
         leaf.find("rst") != std::string::npos || leaf.find("reset") != std::string::npos;
}

std::string ClassifyGlobalNetCategory(std::string_view path) {
  std::string leaf = LeafName(std::string(path));
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

std::vector<std::string> CollectRhsSignals(const slang::ast::AssignmentExpression *assignment) {
  if (assignment == nullptr) return {};

  class RhsSignalCollector : public slang::ast::ASTVisitor<RhsSignalCollector, true, true> {
   public:
    explicit RhsSignalCollector(std::vector<std::string> &out) : out_(out) {}

    void handle(const slang::ast::NamedValueExpression &nve) {
      if (!IsTraceable(&nve.symbol)) return;
      out_.push_back(std::string(nve.symbol.getHierarchicalPath()));
    }

   private:
    std::vector<std::string> &out_;
  };

  std::vector<std::string> paths;
  RhsSignalCollector collector(paths);
  assignment->right().visit(collector);
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

std::vector<std::string> CollectLhsSignals(const slang::ast::AssignmentExpression *assignment) {
  if (assignment == nullptr) return {};

  class LhsSignalCollector : public slang::ast::ASTVisitor<LhsSignalCollector, true, true> {
   public:
    explicit LhsSignalCollector(std::vector<std::string> &out) : out_(out) {}

    void handle(const slang::ast::NamedValueExpression &nve) {
      if (!IsTraceable(&nve.symbol)) return;
      out_.push_back(std::string(nve.symbol.getHierarchicalPath()));
    }

   private:
    std::vector<std::string> &out_;
  };

  std::vector<std::string> paths;
  LhsSignalCollector collector(paths);
  assignment->left().visit(collector);
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

std::vector<std::string> CollectLhsSignalsFromStatement(const slang::ast::Statement &stmt) {
  class StatementLhsCollector
      : public slang::ast::ASTVisitor<StatementLhsCollector, true, true> {
   public:
    explicit StatementLhsCollector(std::vector<std::string> &out) : out_(out) {}

    void handle(const slang::ast::AssignmentExpression &assignment) {
      class LhsCollector : public slang::ast::ASTVisitor<LhsCollector, true, true> {
       public:
        explicit LhsCollector(std::vector<std::string> &out) : out_(out) {}

        void handle(const slang::ast::NamedValueExpression &nve) {
          if (!IsTraceable(&nve.symbol)) return;
          out_.push_back(std::string(nve.symbol.getHierarchicalPath()));
        }

       private:
        std::vector<std::string> &out_;
      };
      LhsCollector lhs_collector(out_);
      assignment.left().visit(lhs_collector);
      assignment.right().visit(*this);
    }

   private:
    std::vector<std::string> &out_;
  };

  std::vector<std::string> paths;
  StatementLhsCollector collector(paths);
  stmt.visit(collector);
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

const std::vector<std::string> &GetCachedLhsSignals(
    const slang::ast::AssignmentExpression *assignment, TraceCompileCache &cache) {
  static const std::vector<std::string> empty;
  if (assignment == nullptr) return empty;
  auto it = cache.assignment_lhs_signals.find(assignment);
  if (it != cache.assignment_lhs_signals.end()) return it->second;
  return cache.assignment_lhs_signals.emplace(assignment, CollectLhsSignals(assignment)).first->second;
}

const std::vector<std::string> &GetCachedRhsSignals(
    const slang::ast::AssignmentExpression *assignment, TraceCompileCache &cache) {
  static const std::vector<std::string> empty;
  if (assignment == nullptr) return empty;
  auto it = cache.assignment_rhs_signals.find(assignment);
  if (it != cache.assignment_rhs_signals.end()) return it->second;
  return cache.assignment_rhs_signals.emplace(assignment, CollectRhsSignals(assignment)).first->second;
}

const std::vector<std::string> &GetCachedStatementLhsSignals(
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
          rec.path = item.expr->symbol.getHierarchicalPath();
          const auto loc = item.expr->sourceRange.start();
          rec.file = std::string(sm.getFileName(loc));
          rec.line = sm.getLineNumber(loc);
          auto bit_desc = DescribeBitSelectors(item.selectors, sm, item.expr->symbol);
          rec.bit_map = std::move(bit_desc.first);
          rec.bit_map_approximate = bit_desc.second;
          if (item.assignment != nullptr) {
            if (auto off = GetSourceOffsetRange(item.assignment->sourceRange, sm); off.has_value()) {
              rec.has_assignment_range = true;
              rec.assignment_start = off->first;
              rec.assignment_end = off->second;
            }
            if (cache != nullptr) {
              rec.lhs_signals = GetCachedLhsSignals(item.assignment, *cache);
              rec.rhs_signals = GetCachedRhsSignals(item.assignment, *cache);
            } else {
              rec.lhs_signals = CollectLhsSignals(item.assignment);
              rec.rhs_signals = CollectRhsSignals(item.assignment);
            }
          } else if (!item.context_lhs_signals.empty()) {
            rec.lhs_signals = item.context_lhs_signals;
          }
          if (item.context_from_instance_port && !item.context_instance_port_signal.empty()) {
            if (drivers_mode) {
              if (std::find(rec.rhs_signals.begin(), rec.rhs_signals.end(),
                            item.context_instance_port_signal) == rec.rhs_signals.end()) {
                rec.rhs_signals.push_back(item.context_instance_port_signal);
              }
            } else {
              if (std::find(rec.lhs_signals.begin(), rec.lhs_signals.end(),
                            item.context_instance_port_signal) == rec.lhs_signals.end()) {
                rec.lhs_signals.push_back(item.context_instance_port_signal);
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
                             std::unordered_map<std::string, const slang::ast::Symbol *> &out) {
  auto collect_from_scope = [&](const slang::ast::Scope &scope) {
    for (const auto &net : scope.membersOfType<slang::ast::NetSymbol>()) {
      out.try_emplace(std::string(net.getHierarchicalPath()), &net);
    }
    for (const auto &var : scope.membersOfType<slang::ast::VariableSymbol>()) {
      out.try_emplace(std::string(var.getHierarchicalPath()), &var);
    }
  };

  for (const slang::ast::InstanceSymbol *top : root.topInstances) {
    collect_from_scope(top->body);
    top->visit(slang::ast::makeVisitor(
        [&](auto &visitor, const slang::ast::InstanceSymbol &inst) {
          collect_from_scope(inst.body);
          visitor.visitDefault(inst);
        },
        [&](auto &visitor, const slang::ast::GenerateBlockSymbol &gen) {
          if (gen.isUninstantiated) return;
          collect_from_scope(gen);
          visitor.visitDefault(gen);
        }));
  }
}

void CollectInstanceHierarchy(const slang::ast::RootSymbol &root, TraceDb &db) {
  std::unordered_map<std::string, std::string> modules;
  auto note_instance = [&](const slang::ast::InstanceSymbol &inst) {
    modules.try_emplace(std::string(inst.getHierarchicalPath()),
                        std::string(inst.getDefinition().name));
  };

  for (const slang::ast::InstanceSymbol *top : root.topInstances) {
    note_instance(*top);
    top->visit(slang::ast::makeVisitor(
        [&](auto &visitor, const slang::ast::InstanceSymbol &inst) {
          note_instance(inst);
          visitor.visitDefault(inst);
        },
        [&](auto &visitor, const slang::ast::GenerateBlockSymbol &gen) {
          if (gen.isUninstantiated) return;
          visitor.visitDefault(gen);
        }));
  }

  for (const auto &[path, module] : modules) {
    auto &node = db.hierarchy[path];
    node.module = module;
  }
  for (const auto &[path, _] : modules) {
    const std::string parent = ParentPath(path);
    if (parent.empty()) continue;
    auto parent_it = db.hierarchy.find(parent);
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
    std::string cur = ParentPath(sig);
    while (!cur.empty()) {
      db.hierarchy.try_emplace(cur, HierNodeRecord{});
      cur = ParentPath(cur);
    }
  }
  for (const auto &[path, _] : db.hierarchy) {
    const std::string parent = ParentPath(path);
    if (parent.empty()) continue;
    auto it = db.hierarchy.find(parent);
    if (it == db.hierarchy.end()) continue;
    it->second.children.push_back(path);
  }
  for (auto &[_, node] : db.hierarchy) {
    std::sort(node.children.begin(), node.children.end());
    node.children.erase(std::unique(node.children.begin(), node.children.end()), node.children.end());
  }
}

std::string EscapeField(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '\\') {
      out += "\\\\";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c == '\n') {
      out += "\\n";
    } else {
      out += c;
    }
  }
  return out;
}

std::vector<std::string> SplitEscapedTsv(std::string_view line) {
  std::vector<std::string> fields;
  std::string cur;
  bool escaped = false;
  for (char c : line) {
    if (escaped) {
      if (c == 't') {
        cur += '\t';
      } else if (c == 'n') {
        cur += '\n';
      } else {
        cur += c;
      }
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
    } else if (c == '\t') {
      fields.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  fields.push_back(cur);
  return fields;
}

struct V7WriteStringIndex {
  std::unordered_map<std::string, uint32_t> prefix_to_id;
  std::unordered_map<std::string, uint32_t> file_to_id;
  std::vector<std::string> prefix_by_id;
  std::vector<std::string> file_by_id;
};

void WriteEndpointLine(std::ofstream &out, char kind, const EndpointRecord &e, const V7WriteStringIndex &index,
                       const TraceDb *db);
void CollectEndpointStrings(const EndpointRecord &e, V7WriteStringIndex &index, const TraceDb *db);
void WriteV7StringTables(std::ofstream &out, const V7WriteStringIndex &index);
uint32_t RegisterPrefix(V7WriteStringIndex &index, const std::string &prefix);
uint32_t RegisterFile(V7WriteStringIndex &index, const std::string &file);
std::string EncodePathToken(const std::string &path, const V7WriteStringIndex &index);

bool SaveDb(const std::string &db_path, const TraceDb &db) {
  std::ofstream out(db_path);
  if (!out.is_open()) return false;

  V7WriteStringIndex string_index;
  std::vector<std::string> signal_names;
  signal_names.reserve(db.signals.size());
  for (const auto &[name, _] : db.signals) {
    signal_names.push_back(name);
  }
  std::sort(signal_names.begin(), signal_names.end());

  for (const std::string &name : signal_names) {
    const auto it = db.signals.find(name);
    if (it == db.signals.end()) continue;
    const auto [sig_prefix, _sig_leaf] = SplitPathPrefixLeaf(name);
    RegisterPrefix(string_index, sig_prefix);
    const std::vector<EndpointRecord> drivers = MergeEndpointBitRanges(it->second.drivers);
    const std::vector<EndpointRecord> loads = MergeEndpointBitRanges(it->second.loads);
    for (const EndpointRecord &e : drivers)
      CollectEndpointStrings(e, string_index, &db);
    for (const EndpointRecord &e : loads)
      CollectEndpointStrings(e, string_index, &db);
  }
  for (const auto &[path, node] : db.hierarchy) {
    const auto [pfx, _leaf] = SplitPathPrefixLeaf(path);
    RegisterPrefix(string_index, pfx);
    for (const std::string &ch : node.children) {
      const auto [cpfx, _cleaf] = SplitPathPrefixLeaf(ch);
      RegisterPrefix(string_index, cpfx);
    }
  }

  out << "RTL_TRACE_DB_V8\n";
  WriteV7StringTables(out, string_index);

  for (const std::string &name : signal_names) {
    const auto it = db.signals.find(name);
    if (it == db.signals.end()) continue;
    const auto [sig_prefix, sig_leaf] = SplitPathPrefixLeaf(name);
    const auto pit = string_index.prefix_to_id.find(sig_prefix);
    if (pit == string_index.prefix_to_id.end()) continue;
    const std::vector<EndpointRecord> drivers = MergeEndpointBitRanges(it->second.drivers);
    const std::vector<EndpointRecord> loads = MergeEndpointBitRanges(it->second.loads);
    out << "S\t" << pit->second << '\t' << EscapeField(sig_leaf) << '\n';
    for (const EndpointRecord &e : drivers)
      WriteEndpointLine(out, 'D', e, string_index, &db);
    for (const EndpointRecord &e : loads)
      WriteEndpointLine(out, 'L', e, string_index, &db);
    out << "X\n";
  }
  std::vector<std::string> hier_paths;
  hier_paths.reserve(db.hierarchy.size());
  for (const auto &[path, _] : db.hierarchy)
    hier_paths.push_back(path);
  std::sort(hier_paths.begin(), hier_paths.end());
  for (const std::string &path : hier_paths) {
    const auto it = db.hierarchy.find(path);
    if (it == db.hierarchy.end()) continue;
    const auto [pfx, leaf] = SplitPathPrefixLeaf(path);
    const auto pit = string_index.prefix_to_id.find(pfx);
    if (pit == string_index.prefix_to_id.end()) continue;
    std::vector<std::string> enc_children;
    enc_children.reserve(it->second.children.size());
    for (const std::string &ch : it->second.children)
      enc_children.push_back(EncodePathToken(ch, string_index));
    const std::string children_joined = JoinLines(enc_children);
    out << "I\t" << pit->second << '\t' << EscapeField(leaf) << '\t' << EscapeField(it->second.module) << '\t'
        << EscapeField(children_joined) << '\n';
  }
  std::vector<std::string> global_sources;
  global_sources.reserve(db.global_nets.size());
  for (const auto &[path, _] : db.global_nets)
    global_sources.push_back(path);
  std::sort(global_sources.begin(), global_sources.end());
  for (const std::string &source : global_sources) {
    const auto it = db.global_nets.find(source);
    if (it == db.global_nets.end()) continue;
    out << "G\t" << EscapeField(source) << '\t' << EscapeField(it->second.category) << '\t'
        << EscapeField(JoinLines(it->second.sinks)) << '\n';
  }
  return true;
}

bool IsUnderHierarchyRoot(const std::string &signal, const std::string &root) {
  if (root.empty()) return true;
  if (signal == root) return true;
  if (signal.size() <= root.size()) return false;
  if (signal.rfind(root, 0) != 0) return false;
  return signal[root.size()] == '.';
}

std::unordered_map<std::string, size_t> BuildSubtreeSignalCounts(
    const std::vector<std::string> &keys) {
  std::unordered_map<std::string, size_t> counts;
  for (const std::string &sig : keys) {
    std::string inst = ParentPath(sig);
    while (!inst.empty()) {
      counts[inst] += 1;
      inst = ParentPath(inst);
    }
  }
  return counts;
}

std::vector<PartitionRecord> PlanHierarchyPartitions(
    const TraceDb &hier_db, const std::unordered_map<std::string, size_t> &subtree_counts,
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

uint32_t RegisterPrefix(V7WriteStringIndex &index, const std::string &prefix) {
  auto it = index.prefix_to_id.find(prefix);
  if (it != index.prefix_to_id.end()) return it->second;
  const uint32_t id = static_cast<uint32_t>(index.prefix_by_id.size());
  index.prefix_to_id.emplace(prefix, id);
  index.prefix_by_id.push_back(prefix);
  return id;
}

uint32_t RegisterFile(V7WriteStringIndex &index, const std::string &file) {
  auto it = index.file_to_id.find(file);
  if (it != index.file_to_id.end()) return it->second;
  const uint32_t id = static_cast<uint32_t>(index.file_by_id.size());
  index.file_to_id.emplace(file, id);
  index.file_by_id.push_back(file);
  return id;
}

void WriteEndpointLine(std::ofstream &out, char kind, const EndpointRecord &e, const V7WriteStringIndex &index,
                       const TraceDb *db) {
  const std::string &path = (db == nullptr) ? e.path : EndpointPath(*db, e);
  const std::string &file = (db == nullptr) ? e.file : EndpointFile(*db, e);
  const auto [prefix, leaf] = SplitPathPrefixLeaf(path);
  auto pit = index.prefix_to_id.find(prefix);
  auto fit = index.file_to_id.find(file);
  if (pit == index.prefix_to_id.end() || fit == index.file_to_id.end()) return;
  const uint32_t prefix_id = pit->second;
  const uint32_t file_id = fit->second;
  out << kind << '\t' << (e.kind == EndpointKind::kPort ? 'P' : 'E') << '\t' << prefix_id << '\t'
      << EscapeField(leaf) << '\t' << file_id << '\t' << e.line << '\t' << EscapeField(e.direction)
      << '\t' << EscapeField(e.bit_map) << '\t' << (e.bit_map_approximate ? "1" : "0");
  if (e.has_assignment_range || !e.assignment_text.empty() || !e.lhs_signals.empty() || !e.rhs_signals.empty()) {
    std::string lhs_joined;
    for (size_t i = 0; i < e.lhs_signals.size(); ++i) {
      if (i != 0) lhs_joined += '\n';
      lhs_joined += EncodePathToken(e.lhs_signals[i], index);
    }
    std::string rhs_joined;
    for (size_t i = 0; i < e.rhs_signals.size(); ++i) {
      if (i != 0) rhs_joined += '\n';
      rhs_joined += EncodePathToken(e.rhs_signals[i], index);
    }
    out << '\t' << (e.has_assignment_range ? "1" : "0") << '\t' << e.assignment_start << '\t'
        << e.assignment_end << '\t' << EscapeField(lhs_joined) << '\t' << EscapeField(rhs_joined);
  }
  out << '\n';
}

void CollectEndpointStrings(const EndpointRecord &e, V7WriteStringIndex &index, const TraceDb *db) {
  const std::string &path = (db == nullptr) ? e.path : EndpointPath(*db, e);
  const std::string &file = (db == nullptr) ? e.file : EndpointFile(*db, e);
  const auto [prefix, _] = SplitPathPrefixLeaf(path);
  RegisterPrefix(index, prefix);
  RegisterFile(index, file);
  for (const std::string &sig : e.lhs_signals) {
    const auto [pfx, _leaf] = SplitPathPrefixLeaf(sig);
    RegisterPrefix(index, pfx);
  }
  for (const std::string &sig : e.rhs_signals) {
    const auto [pfx, _leaf] = SplitPathPrefixLeaf(sig);
    RegisterPrefix(index, pfx);
  }
}

void WriteV7StringTables(std::ofstream &out, const V7WriteStringIndex &index) {
  for (size_t i = 0; i < index.prefix_by_id.size(); ++i) {
    const std::string &prefix = index.prefix_by_id[i];
    out << "P\t" << i << '\t' << Fnv1a64(prefix) << '\t' << EscapeField(prefix) << '\n';
  }
  for (size_t i = 0; i < index.file_by_id.size(); ++i) {
    const std::string &file = index.file_by_id[i];
    out << "F\t" << i << '\t' << Fnv1a64(file) << '\t' << EscapeField(file) << '\n';
  }
}

std::string EncodePathToken(const std::string &path, const V7WriteStringIndex &index) {
  const auto [prefix, leaf] = SplitPathPrefixLeaf(path);
  const auto it = index.prefix_to_id.find(prefix);
  if (it == index.prefix_to_id.end()) return path;
  return std::to_string(it->second) + ":" + leaf;
}

void WriteGlobalNetLines(std::ofstream &out, const TraceDb &db) {
  std::vector<std::string> global_sources;
  global_sources.reserve(db.global_nets.size());
  for (const auto &[path, _] : db.global_nets)
    global_sources.push_back(path);
  std::sort(global_sources.begin(), global_sources.end());
  for (const std::string &source : global_sources) {
    const auto it = db.global_nets.find(source);
    if (it == db.global_nets.end()) continue;
    out << "G\t" << EscapeField(source) << '\t' << EscapeField(it->second.category) << '\t'
        << EscapeField(JoinLines(it->second.sinks)) << '\n';
  }
}

bool SaveDbStreaming(const std::string &db_path, const std::vector<std::string> &keys,
                     const std::unordered_map<std::string, const slang::ast::Symbol *> &symbols,
                     const slang::SourceManager &sm, const TraceDb &hier_db,
                     const std::vector<PartitionRecord> &parts,
                     const std::vector<std::vector<size_t>> &buckets, size_t &signal_count,
                     CompileLogger *logger) {
  using Clock = std::chrono::steady_clock;
  auto fmt_seconds = [](const Clock::time_point &start, const Clock::time_point &end) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(3)
       << std::chrono::duration<double>(end - start).count();
    return os.str();
  };
  const auto t_total_start = Clock::now();
  if (logger != nullptr) {
    logger->Log("save_db_streaming: begin keys=" + std::to_string(keys.size()) +
                " hier_nodes=" + std::to_string(hier_db.hierarchy.size()) +
                " partitions=" + std::to_string(parts.size()));
  }

  std::ofstream out(db_path);
  if (!out.is_open()) return false;
  V7WriteStringIndex string_index;
  TraceCompileCache trace_cache;
  auto build_signal_record = [&](const slang::ast::Symbol *sym) -> const SignalRecord & {
    return GetOrBuildSignalRecord(sym, sm, trace_cache);
  };
  auto collect_strings_for_signal = [&](const std::string &path) {
    auto it = symbols.find(path);
    if (it == symbols.end() || !IsTraceable(it->second)) return;
    const auto [sig_prefix, _sig_leaf] = SplitPathPrefixLeaf(path);
    RegisterPrefix(string_index, sig_prefix);
    SignalRecord rec = build_signal_record(it->second);
    if (ShouldCompactGlobalNet(path, rec.loads.size())) {
      GlobalNetRecord g;
      g.category = ClassifyGlobalNetCategory(path);
      g.sinks = ExtractCompactSinkPaths(path, rec.loads);
      if (!g.sinks.empty()) rec.loads.clear();
    }
    std::vector<EndpointRecord> drivers = MergeEndpointBitRanges(std::move(rec.drivers));
    std::vector<EndpointRecord> loads = MergeEndpointBitRanges(std::move(rec.loads));
    for (const EndpointRecord &e : drivers)
      CollectEndpointStrings(e, string_index, nullptr);
    for (const EndpointRecord &e : loads)
      CollectEndpointStrings(e, string_index, nullptr);
  };

  const auto t_collect_start = Clock::now();
  if (parts.empty()) {
    for (size_t ki : buckets.front())
      collect_strings_for_signal(keys[ki]);
  } else {
    for (size_t p = 0; p < parts.size(); ++p) {
      for (size_t ki : buckets[p])
        collect_strings_for_signal(keys[ki]);
    }
  }
  for (const auto &[path, node] : hier_db.hierarchy) {
    const auto [pfx, _leaf] = SplitPathPrefixLeaf(path);
    RegisterPrefix(string_index, pfx);
    for (const std::string &ch : node.children) {
      const auto [cpfx, _cleaf] = SplitPathPrefixLeaf(ch);
      RegisterPrefix(string_index, cpfx);
    }
  }
  const auto t_collect_end = Clock::now();
  if (logger != nullptr) {
    logger->Log("save_db_streaming: collect_strings done elapsed_s=" +
                fmt_seconds(t_collect_start, t_collect_end) +
                " prefixes=" + std::to_string(string_index.prefix_by_id.size()) +
                " files=" + std::to_string(string_index.file_by_id.size()));
  }

  const auto t_header_start = Clock::now();
  out << "RTL_TRACE_DB_V8\n";
  WriteV7StringTables(out, string_index);
  const auto t_header_end = Clock::now();
  if (logger != nullptr) {
    logger->Log("save_db_streaming: write_header_tables done elapsed_s=" +
                fmt_seconds(t_header_start, t_header_end));
  }

  signal_count = 0;
  size_t emit_items_total = 0;
  for (const auto &bucket : buckets)
    emit_items_total += bucket.size();
  size_t emit_items_done = 0;
  const auto t_emit_start = Clock::now();
  auto t_emit_last_log = t_emit_start;
  auto maybe_log_emit_progress = [&](bool force) {
    if (logger == nullptr) return;
    const auto now = Clock::now();
    const bool enough_time = std::chrono::duration_cast<std::chrono::seconds>(now - t_emit_last_log).count() >= 30;
    const bool enough_items = (emit_items_done != 0 && (emit_items_done % 10000 == 0));
    if (!force && !enough_time && !enough_items) return;
    t_emit_last_log = now;
    std::string pct = "n/a";
    if (emit_items_total != 0) {
      std::ostringstream os;
      os << std::fixed << std::setprecision(2)
         << (100.0 * static_cast<double>(emit_items_done) / static_cast<double>(emit_items_total));
      pct = os.str();
    }
    logger->Log("save_db_streaming: emit progress elapsed_s=" +
                fmt_seconds(t_emit_start, now) + " items=" + std::to_string(emit_items_done) +
                "/" + std::to_string(emit_items_total) + " (" + pct + "%) written_signals=" +
                std::to_string(signal_count));
  };
  TraceDb compact_db;
  size_t compacted_net_count = 0;
  size_t compacted_sink_count = 0;
  if (parts.empty()) {
    if (logger != nullptr) logger->Log("db emit: single partition mode");
    for (size_t ki : buckets.front()) {
      ++emit_items_done;
      const std::string &path = keys[ki];
      auto it = symbols.find(path);
      if (it == symbols.end() || !IsTraceable(it->second)) continue;
      SignalRecord rec = build_signal_record(it->second);
      if (ShouldCompactGlobalNet(path, rec.loads.size())) {
        GlobalNetRecord g;
        g.category = ClassifyGlobalNetCategory(path);
        g.sinks = ExtractCompactSinkPaths(path, rec.loads);
        if (!g.sinks.empty()) {
          compacted_sink_count += g.sinks.size();
          ++compacted_net_count;
          for (const std::string &sink : g.sinks)
            compact_db.global_sink_to_source[sink] = path;
          compact_db.global_nets.emplace(path, std::move(g));
          rec.loads.clear();
        }
      }
      rec.drivers = MergeEndpointBitRanges(std::move(rec.drivers));
      rec.loads = MergeEndpointBitRanges(std::move(rec.loads));
      const auto [sig_prefix, sig_leaf] = SplitPathPrefixLeaf(path);
      const auto pit = string_index.prefix_to_id.find(sig_prefix);
      if (pit == string_index.prefix_to_id.end()) continue;
      out << "S\t" << pit->second << '\t' << EscapeField(sig_leaf) << '\n';
      for (const EndpointRecord &e : rec.drivers)
        WriteEndpointLine(out, 'D', e, string_index, nullptr);
      for (const EndpointRecord &e : rec.loads)
        WriteEndpointLine(out, 'L', e, string_index, nullptr);
      out << "X\n";
      ++signal_count;
      maybe_log_emit_progress(false);
    }
  } else {
    for (size_t p = 0; p < parts.size(); ++p) {
      if (logger != nullptr) {
        logger->Log("db emit: partition " + std::to_string(p + 1) + "/" + std::to_string(parts.size()) +
                    " root=" + parts[p].root + " planned_signals=" +
                    std::to_string(parts[p].signal_count) + " assigned=" +
                    std::to_string(buckets[p].size()));
      }
      for (size_t ki : buckets[p]) {
        ++emit_items_done;
        const std::string &path = keys[ki];
        auto it = symbols.find(path);
        if (it == symbols.end() || !IsTraceable(it->second)) continue;
        SignalRecord rec = build_signal_record(it->second);
        if (ShouldCompactGlobalNet(path, rec.loads.size())) {
          GlobalNetRecord g;
          g.category = ClassifyGlobalNetCategory(path);
          g.sinks = ExtractCompactSinkPaths(path, rec.loads);
          if (!g.sinks.empty()) {
            compacted_sink_count += g.sinks.size();
            ++compacted_net_count;
            for (const std::string &sink : g.sinks)
              compact_db.global_sink_to_source[sink] = path;
            compact_db.global_nets.emplace(path, std::move(g));
            rec.loads.clear();
          }
        }
        rec.drivers = MergeEndpointBitRanges(std::move(rec.drivers));
        rec.loads = MergeEndpointBitRanges(std::move(rec.loads));
        const auto [sig_prefix, sig_leaf] = SplitPathPrefixLeaf(path);
        const auto pit = string_index.prefix_to_id.find(sig_prefix);
        if (pit == string_index.prefix_to_id.end()) continue;
        out << "S\t" << pit->second << '\t' << EscapeField(sig_leaf) << '\n';
        for (const EndpointRecord &e : rec.drivers)
          WriteEndpointLine(out, 'D', e, string_index, nullptr);
        for (const EndpointRecord &e : rec.loads)
          WriteEndpointLine(out, 'L', e, string_index, nullptr);
        out << "X\n";
        ++signal_count;
        maybe_log_emit_progress(false);
      }
    }
  }
  maybe_log_emit_progress(true);
  const auto t_emit_end = Clock::now();
  if (logger != nullptr) {
    logger->Log("save_db_streaming: emit_signals done elapsed_s=" +
                fmt_seconds(t_emit_start, t_emit_end) +
                " written_signals=" + std::to_string(signal_count));
  }

  const auto t_hier_start = Clock::now();
  std::vector<std::string> hier_paths;
  hier_paths.reserve(hier_db.hierarchy.size());
  for (const auto &[path, _] : hier_db.hierarchy)
    hier_paths.push_back(path);
  std::sort(hier_paths.begin(), hier_paths.end());
  for (const std::string &path : hier_paths) {
    const auto it = hier_db.hierarchy.find(path);
    if (it == hier_db.hierarchy.end()) continue;
    const auto [pfx, leaf] = SplitPathPrefixLeaf(path);
    const auto pit = string_index.prefix_to_id.find(pfx);
    if (pit == string_index.prefix_to_id.end()) continue;
    std::vector<std::string> enc_children;
    enc_children.reserve(it->second.children.size());
    for (const std::string &ch : it->second.children)
      enc_children.push_back(EncodePathToken(ch, string_index));
    const std::string children_joined = JoinLines(enc_children);
    out << "I\t" << pit->second << '\t' << EscapeField(leaf) << '\t' << EscapeField(it->second.module) << '\t'
        << EscapeField(children_joined) << '\n';
  }
  const auto t_hier_end = Clock::now();
  if (logger != nullptr) {
    logger->Log("save_db_streaming: write_hierarchy done elapsed_s=" +
                fmt_seconds(t_hier_start, t_hier_end) +
                " nodes=" + std::to_string(hier_paths.size()));
  }

  const auto t_global_start = Clock::now();
  WriteGlobalNetLines(out, compact_db);
  const auto t_global_end = Clock::now();
  if (logger != nullptr) {
    logger->Log("save_db_streaming: write_global_nets done elapsed_s=" +
                fmt_seconds(t_global_start, t_global_end) +
                " nets=" + std::to_string(compact_db.global_nets.size()));
  }
  if (logger != nullptr && compacted_net_count != 0) {
    logger->Log("global-net compaction: nets=" + std::to_string(compacted_net_count) +
                " sinks=" + std::to_string(compacted_sink_count));
  }
  if (logger != nullptr) {
    logger->Log("save_db_streaming: done elapsed_s=" +
                fmt_seconds(t_total_start, Clock::now()));
  }
  return true;
}

bool LoadDb(const std::string &db_path, TraceDb &db) {
  std::ifstream in(db_path);
  if (!in.is_open()) return false;
  db.db_dir = std::filesystem::path(db_path).parent_path().string();

  std::string line;
  if (!std::getline(in, line)) return false;
  const bool is_v7 = (line == "RTL_TRACE_DB_V7");
  const bool is_v8 = (line == "RTL_TRACE_DB_V8");
  if (!is_v7 && !is_v8) return false;

  SignalRecord *current = nullptr;
  std::vector<std::string> prefix_dict;
  std::vector<std::string> file_dict;
  std::unordered_map<std::string, uint32_t> path_index;
  std::unordered_map<std::string, uint32_t> file_index;
  auto decode_path_token = [&](const std::string &token) -> std::string {
    const size_t sep = token.find(':');
    if (sep == std::string::npos) return token;
    try {
      const size_t pid = static_cast<size_t>(std::stoul(token.substr(0, sep)));
      if (pid >= prefix_dict.size()) return token;
      const std::string leaf = token.substr(sep + 1);
      if (prefix_dict[pid].empty()) return leaf;
      return prefix_dict[pid] + "." + leaf;
    } catch (...) { return token; }
  };
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    const std::vector<std::string> fields = SplitEscapedTsv(line);
    if (fields.empty()) continue;
    if (fields[0] == "P") {
      if (fields.size() < 4) return false;
      const size_t id = static_cast<size_t>(std::stoul(fields[1]));
      if (id >= prefix_dict.size()) prefix_dict.resize(id + 1);
      prefix_dict[id] = fields[3];
      continue;
    }
    if (fields[0] == "F") {
      if (fields.size() < 4) return false;
      const size_t id = static_cast<size_t>(std::stoul(fields[1]));
      if (id >= file_dict.size()) file_dict.resize(id + 1);
      file_dict[id] = fields[3];
      continue;
    }
    if (fields[0] == "S") {
      std::string signal_name;
      if (fields.size() == 2) {
        signal_name = fields[1];
      } else if (fields.size() == 3) {
        const size_t prefix_id = static_cast<size_t>(std::stoul(fields[1]));
        if (prefix_id >= prefix_dict.size()) return false;
        signal_name = prefix_dict[prefix_id].empty() ? fields[2] : (prefix_dict[prefix_id] + "." + fields[2]);
      } else {
        return false;
      }
      auto [it, inserted] = db.signals.emplace(signal_name, SignalRecord{});
      current = &it->second;
      continue;
    }
    if (fields[0] == "X") {
      current = nullptr;
      continue;
    }
    if ((fields[0] == "D" || fields[0] == "L") && current != nullptr) {
      if (fields.size() < 9) return false;
      EndpointRecord e;
      e.kind = fields[1] == "P" ? EndpointKind::kPort : EndpointKind::kExpr;
      const size_t prefix_id = static_cast<size_t>(std::stoul(fields[2]));
      const std::string &leaf = fields[3];
      const size_t file_id = static_cast<size_t>(std::stoul(fields[4]));
      if (prefix_id >= prefix_dict.size() || file_id >= file_dict.size()) return false;
      const std::string &prefix = prefix_dict[prefix_id];
      const std::string full_path = prefix.empty() ? leaf : (prefix + "." + leaf);
      e.path_id = InternString(full_path, db.path_pool, path_index);
      e.file_id = InternString(file_dict[file_id], db.file_pool, file_index);
      e.line = std::stoi(fields[5]);
      e.direction = fields[6];
      size_t next_field = 7;
      if (fields.size() > next_field) e.bit_map = fields[next_field];
      if (fields.size() > next_field + 1) e.bit_map_approximate = (fields[next_field + 1] == "1");
      next_field += 2;
      if (is_v8) {
        if (fields.size() > next_field) e.has_assignment_range = (fields[next_field] == "1");
        if (fields.size() > next_field + 1) e.assignment_start = static_cast<uint32_t>(std::stoul(fields[next_field + 1]));
        if (fields.size() > next_field + 2) e.assignment_end = static_cast<uint32_t>(std::stoul(fields[next_field + 2]));
        if (fields.size() > next_field + 3 && !fields[next_field + 3].empty()) {
          e.lhs_signals = SplitJoinedField(fields[next_field + 3]);
          for (std::string &sig : e.lhs_signals)
            sig = decode_path_token(sig);
        }
        if (fields.size() > next_field + 4 && !fields[next_field + 4].empty()) {
          e.rhs_signals = SplitJoinedField(fields[next_field + 4]);
          for (std::string &sig : e.rhs_signals)
            sig = decode_path_token(sig);
        }
      } else {
        if (fields.size() > next_field) {
          e.assignment_text = fields[next_field];
        }
        if (fields.size() > next_field + 1 && !fields[next_field + 1].empty()) {
          e.lhs_signals = SplitJoinedField(fields[next_field + 1]);
          for (std::string &sig : e.lhs_signals)
            sig = decode_path_token(sig);
        }
        const size_t rhs_index = next_field + 2;
        if (fields.size() > rhs_index && !fields[rhs_index].empty()) {
          e.rhs_signals = SplitJoinedField(fields[rhs_index]);
          for (std::string &sig : e.rhs_signals)
            sig = decode_path_token(sig);
        }
      }
      if (fields[0] == "D") {
        current->drivers.push_back(std::move(e));
      } else {
        current->loads.push_back(std::move(e));
      }
      continue;
    }
    if (fields[0] == "I") {
      std::string node_path;
      size_t module_index = 2;
      size_t children_index = 3;
      if (fields.size() >= 4) {
        bool compressed = false;
        size_t consumed = 0;
        size_t prefix_id = 0;
        try {
          prefix_id = static_cast<size_t>(std::stoul(fields[1], &consumed));
          compressed = (consumed == fields[1].size());
        } catch (...) { compressed = false; }
        if (compressed) {
          if (prefix_id >= prefix_dict.size()) return false;
          node_path = prefix_dict[prefix_id].empty() ? fields[2] : (prefix_dict[prefix_id] + "." + fields[2]);
          module_index = 3;
          children_index = 4;
        } else {
          node_path = fields[1];
        }
      } else if (fields.size() == 2) {
        node_path = fields[1];
      } else {
        return false;
      }
      auto &node = db.hierarchy[node_path];
      if (fields.size() > module_index) node.module = fields[module_index];
      if (fields.size() > children_index && !fields[children_index].empty()) {
        node.children = SplitJoinedField(fields[children_index]);
        for (std::string &child : node.children)
          child = decode_path_token(child);
      }
      continue;
    }
    if (fields[0] == "G") {
      if (fields.size() < 4) return false;
      auto &rec = db.global_nets[fields[1]];
      rec.category = fields[2];
      rec.sinks = SplitJoinedField(fields[3]);
      for (const std::string &sink : rec.sinks) {
        if (!sink.empty()) db.global_sink_to_source[sink] = fields[1];
      }
      continue;
    }
    return false;
  }
  for (auto &[_, node] : db.hierarchy) {
    std::sort(node.children.begin(), node.children.end());
    node.children.erase(std::unique(node.children.begin(), node.children.end()), node.children.end());
  }
  return true;
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

struct MfcuUnitListing {
  std::vector<std::string> files;
  std::vector<std::string> includes;
  std::vector<std::string> defines;
};

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

bool ParseMfcuFlist(const std::filesystem::path &flist, MfcuUnitListing &unit) {
  std::ifstream in(flist);
  if (!in.is_open()) return false;
  const std::filesystem::path base = flist.parent_path();
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())))
      line.erase(line.begin());
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
      line.pop_back();
    if (line.empty() || line[0] == '#') continue;

    std::istringstream iss(line);
    std::vector<std::string> toks;
    for (std::string tok; iss >> tok;)
      toks.push_back(tok);

    for (size_t i = 0; i < toks.size(); ++i) {
      const std::string &tok = toks[i];
      if (tok == "-f" || tok == "-F" || tok == "-v" || tok == "--libfile" || tok == "-y" ||
          tok == "--libdir" || tok == "--libmap" || tok == "-C") {
        // Not supported in a generated unit listing; caller should fallback to original -f.
        return false;
      }
      if (tok == "-I") {
        if (i + 1 >= toks.size()) return false;
        unit.includes.push_back(ToAbsPathString(std::filesystem::path(toks[++i]), base));
        continue;
      }
      if (tok == "-D") {
        if (i + 1 >= toks.size()) return false;
        unit.defines.push_back(toks[++i]);
        continue;
      }
      if (tok.rfind("-I", 0) == 0 && tok.size() > 2) {
        unit.includes.push_back(ToAbsPathString(std::filesystem::path(tok.substr(2)), base));
        continue;
      }
      if (tok.rfind("-D", 0) == 0 && tok.size() > 2) {
        unit.defines.push_back(tok.substr(2));
        continue;
      }
      if (ParsePlusList(tok, "+incdir+", unit.includes, base)) continue;
      if (ParseDefinesPlus(tok, unit.defines)) continue;
      if (LooksLikeOptionToken(tok)) {
        // Unknown option in -f; fallback to original -f handling.
        return false;
      }
      unit.files.push_back(ToAbsPathString(std::filesystem::path(tok), base));
    }
  }
  return true;
}

std::optional<std::filesystem::path> WriteMfcuListing(const MfcuUnitListing &unit) {
  if (unit.files.empty()) return std::nullopt;
  std::error_code ec;
  const std::filesystem::path temp_dir = std::filesystem::temp_directory_path(ec);
  if (ec) return std::nullopt;
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path out = temp_dir / ("rtl_trace_mfcu_" + std::to_string(stamp) + ".f");
  std::ofstream os(out);
  if (!os.is_open()) return std::nullopt;
  for (const std::string &inc : unit.includes)
    os << "+incdir+" << inc << "\n";
  for (const std::string &def : unit.defines)
    os << "+define+" << def << "\n";
  for (const std::string &f : unit.files)
    os << f << "\n";
  return out;
}

std::optional<std::vector<std::string>> BuildMfcuArgs(const std::vector<std::string> &args) {
  std::vector<std::string> out;
  out.reserve(args.size() + 8);
  std::vector<std::string> direct_files;
  const std::filesystem::path cwd = std::filesystem::current_path();

  auto takesValue = [](const std::string &opt) {
    static const std::unordered_set<std::string> k = {
      "--top", "--timescale", "--std", "-y", "--libdir", "-v", "--libfile", "--libmap",
      "-I", "-D", "--compat", "-T", "--timing", "--error-limit", "--warning-limit",
      "--max-lexer-errors", "--max-parse-depth", "--max-hierarchy-depth", "--max-generate-steps"
    };
    return k.find(opt) != k.end();
  };

  for (size_t i = 0; i < args.size(); ++i) {
    const std::string &arg = args[i];
    if ((arg == "-f" || arg == "-F") && i + 1 < args.size()) {
      const std::string fl = args[++i];
      const std::filesystem::path flist_path = ToAbsPathString(std::filesystem::path(fl), cwd);
      MfcuUnitListing unit;
      if (ParseMfcuFlist(flist_path, unit) && !unit.files.empty()) {
        auto listing = WriteMfcuListing(unit);
        if (!listing.has_value()) return std::nullopt;
        out.push_back("-C");
        out.push_back(listing->string());
      }
      else {
        // Fallback: keep original -f when it contains unsupported listing directives.
        out.push_back(arg);
        out.push_back(fl);
      }
      continue;
    }

    if (takesValue(arg) && i + 1 < args.size()) {
      out.push_back(arg);
      out.push_back(args[++i]);
      continue;
    }

    if (!LooksLikeOptionToken(arg)) {
      direct_files.push_back(ToAbsPathString(std::filesystem::path(arg), cwd));
      continue;
    }
    out.push_back(arg);
  }

  if (!direct_files.empty()) {
    MfcuUnitListing direct_unit;
    direct_unit.files = std::move(direct_files);
    auto listing = WriteMfcuListing(direct_unit);
    if (!listing.has_value()) return std::nullopt;
    out.push_back("-C");
    out.push_back(listing->string());
  }

  return out;
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
               "[--max-nodes <N>] [--format <text|json>]\n";
  std::cout << "  rtl_trace find --db <file> --query <text|regex> [--regex] [--limit <N>] "
               "[--format <text|json>]\n";
}

int RunCompile(int argc, char *argv[]) {
  std::string db_path = "rtl_trace.db";
  bool incremental = false;
  bool relax_defparam = false;
  bool mfcu = false;
  size_t partition_budget = 0;
  std::string compile_log_path;
  std::vector<std::string> passthrough_args;

  for (int i = 0; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "Usage: rtl_trace compile [--db <file>] [--incremental] [--relax-defparam] [--mfcu] "
                   "[--partition-budget <N>] [--compile-log <file>] "
                   "[slang source args...]\n";
      return 0;
    }
    if (arg == "--db") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --db\n";
        return 1;
      }
      db_path = argv[++i];
      continue;
    }
    if (arg == "--incremental") {
      incremental = true;
      continue;
    }
    if (arg == "--relax-defparam") {
      relax_defparam = true;
      continue;
    }
    if (arg == "--mfcu") {
      mfcu = true;
      continue;
    }
    if (arg == "--partition-budget") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --partition-budget\n";
        return 1;
      }
      try {
        const long long v = std::stoll(argv[++i]);
        if (v <= 0) {
          std::cerr << "--partition-budget must be >= 1\n";
          return 1;
        }
        partition_budget = static_cast<size_t>(v);
      } catch (...) {
        std::cerr << "Invalid value for --partition-budget\n";
        return 1;
      }
      continue;
    }
    if (arg.rfind("--partition-budget=", 0) == 0) {
      try {
        const long long v = std::stoll(arg.substr(std::string("--partition-budget=").size()));
        if (v <= 0) {
          std::cerr << "--partition-budget must be >= 1\n";
          return 1;
        }
        partition_budget = static_cast<size_t>(v);
      } catch (...) {
        std::cerr << "Invalid value for --partition-budget\n";
        return 1;
      }
      continue;
    }
    if (arg == "--compile-log") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --compile-log\n";
        return 1;
      }
      compile_log_path = argv[++i];
      continue;
    }
    if (arg.rfind("--compile-log=", 0) == 0) {
      compile_log_path = arg.substr(std::string("--compile-log=").size());
      continue;
    }
    passthrough_args.push_back(arg);
  }

  CompileLogger logger(compile_log_path);
  logger.Log("compile begin: db=" + db_path + " incremental=" + (incremental ? "1" : "0") +
             " mfcu=" + (mfcu ? "1" : "0") + " partition_budget=" +
             std::to_string(partition_budget));

  if (!HasTopArg(passthrough_args)) {
    std::cerr << "Missing required option: --top <module>\n";
    return 1;
  }

  std::vector<std::string> slang_args = passthrough_args;
  if (mfcu) {
    logger.Log("step: build grouped mfcu arguments");
    auto transformed = BuildMfcuArgs(passthrough_args);
    if (!transformed.has_value()) {
      std::cerr << "Failed to build grouped MFCU command listings\n";
      return 1;
    }
    slang_args = std::move(*transformed);
  }

  const std::filesystem::path db_path_fs = std::filesystem::path(db_path);
  const std::filesystem::path meta_path = db_path_fs.string() + ".meta";
  std::vector<std::string> fingerprint_args = passthrough_args;
  if (mfcu) fingerprint_args.push_back("--mfcu=grouped-v1");
  const std::string new_fingerprint = ComputeCompileFingerprint(fingerprint_args);
  if (incremental && std::filesystem::exists(db_path_fs) && std::filesystem::exists(meta_path)) {
    logger.Log("step: incremental fingerprint check");
    std::ifstream meta_in(meta_path);
    std::string old_fingerprint((std::istreambuf_iterator<char>(meta_in)),
                                std::istreambuf_iterator<char>());
    if (old_fingerprint == new_fingerprint) {
      logger.Log("incremental cache hit");
      std::cout << "db: " << db_path << "\n";
      std::cout << "signals: incremental-cache-hit\n";
      return 0;
    }
  }

  std::vector<std::string> driver_args;
  driver_args.reserve(slang_args.size() + 4);
  driver_args.emplace_back("rtl_trace_compile");
  if (!HasTimescaleArg(slang_args)) {
    driver_args.emplace_back("--timescale");
    driver_args.emplace_back("1ns/1ps");
  }
  if (!HasUnknownSysNameWarningControl(slang_args)) {
    // Verification-only vendor system calls should not block RTL DB compile.
    driver_args.emplace_back("-Wno-unknown-sys-name");
  }
  for (const std::string &arg : slang_args) {
    if (arg == "-Wempty-body") {
      // Compatibility: many existing flows pass -Wempty-body expecting suppression.
      driver_args.emplace_back("-Wno-empty-body");
      continue;
    }
    if (arg.rfind("-Wempty-body=", 0) == 0) {
      driver_args.emplace_back("-Wno-empty-body=" + arg.substr(std::string("-Wempty-body=").size()));
      continue;
    }
    driver_args.push_back(arg);
  }

  std::vector<char *> driver_argv;
  driver_argv.reserve(driver_args.size());
  for (std::string &arg : driver_args) {
    driver_argv.push_back(arg.data());
  }

  slang::driver::Driver driver;
  driver.addStandardArgs();
  logger.Log("step: parse slang command line");
  if (!driver.parseCommandLine(static_cast<int>(driver_argv.size()), driver_argv.data())) return 1;
  logger.Log("step: process slang options");
  if (!driver.processOptions()) return 1;
  logger.Log("step: parse all sources");
  if (!driver.parseAllSources()) return 1;
  logger.Log("step: create compilation and elaborate");
  std::unique_ptr<slang::ast::Compilation> compilation = driver.createCompilation();
  driver.reportCompilation(*compilation, /*quiet*/ true);
  if (HasBlockingCompileDiagnostics(*compilation, relax_defparam)) {
    if (!driver.reportDiagnostics(/*quiet*/ true)) return 1;
  }

  const slang::ast::RootSymbol &root = compilation->getRoot();
  const slang::SourceManager &sm = *compilation->getSourceManager();

  std::unordered_map<std::string, const slang::ast::Symbol *> symbols;
  logger.Log("step: collect traceable symbols");
  CollectTraceableSymbols(root, symbols);

  std::vector<std::string> keys;
  keys.reserve(symbols.size());
  for (const auto &[k, _] : symbols)
    keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  logger.Log("collected symbols: " + std::to_string(keys.size()));

  TraceDb hier_db;
  logger.Log("step: collect instance hierarchy");
  CollectInstanceHierarchy(root, hier_db);

  std::vector<PartitionRecord> parts;
  std::vector<std::vector<size_t>> buckets;
  if (partition_budget > 0) {
    logger.Log("step: plan partitions");
    const auto subtree_counts = BuildSubtreeSignalCounts(keys);
    parts = PlanHierarchyPartitions(hier_db, subtree_counts, partition_budget, &logger);
    logger.Log("planned partitions: " + std::to_string(parts.size()));
    for (size_t i = 0; i < parts.size(); ++i) {
      logger.Log("partition[" + std::to_string(i) + "] root=" + parts[i].root +
                 " depth=" + std::to_string(parts[i].depth) +
                 " subtree_signals=" + std::to_string(parts[i].signal_count));
    }
    buckets = BucketSignalsByPartitions(keys, parts);
  } else {
    buckets.resize(1);
    for (size_t i = 0; i < keys.size(); ++i)
      buckets[0].push_back(i);
  }

  logger.Log("step: emit db");
  size_t written_signal_count = 0;
  if (!SaveDbStreaming(db_path, keys, symbols, sm, hier_db, parts, buckets, written_signal_count,
                       &logger)) {
    std::cerr << "Failed to write DB: " << db_path << "\n";
    return 1;
  }
  std::ofstream meta_out(meta_path);
  if (meta_out.is_open()) meta_out << new_fingerprint;
  logger.Log("compile done: db=" + db_path + " signals=" + std::to_string(written_signal_count));
  std::cout << "db: " << db_path << "\n";
  std::cout << "signals: " << written_signal_count << "\n";
  return 0;
}

std::vector<std::string> TopSuggestions(const TraceDb &db, const std::string &needle, size_t limit) {
  std::vector<std::pair<size_t, std::string>> scored;
  scored.reserve(db.signals.size());
  for (const auto &[name, _] : db.signals) {
    scored.push_back({EditDistance(name, needle), name});
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

std::string FetchSourceSlice(const TraceDb &db, const EndpointRecord &e,
                             std::unordered_map<std::string, std::string> &cache) {
  if (!e.has_assignment_range || e.assignment_end <= e.assignment_start) return "";
  const std::string resolved = ResolveSourcePath(db, EndpointFile(db, e));
  auto it = cache.find(resolved);
  if (it == cache.end()) {
    std::ifstream in(resolved);
    if (!in.is_open()) return "";
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    it = cache.emplace(resolved, std::move(data)).first;
  }
  const std::string &text = it->second;
  const size_t start = static_cast<size_t>(e.assignment_start);
  const size_t end = static_cast<size_t>(e.assignment_end);
  if (start >= text.size() || end > text.size() || end <= start) return "";
  return text.substr(start, end - start);
}

void MaterializeAssignmentTexts(const TraceDb &db, TraceRunResult &result) {
  std::unordered_map<std::string, std::string> file_cache;
  for (EndpointRecord &e : result.endpoints) {
    if (e.kind != EndpointKind::kExpr) continue;
    if (!e.assignment_text.empty()) continue;
    if (!e.has_assignment_range) continue;
    e.assignment_text = FetchSourceSlice(db, e, file_cache);
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

std::optional<TraceRunResult> TryRunGlobalNetFastPath(const TraceDb &db, const TraceOptions &opts) {
  TraceRunResult result;
  if (opts.mode == "drivers") {
    if (!LooksLikeClockOrResetName(opts.root_signal)) return std::nullopt;
    const auto it = db.global_sink_to_source.find(opts.root_signal);
    if (it == db.global_sink_to_source.end()) return std::nullopt;
    EndpointRecord e;
    e.kind = EndpointKind::kExpr;
    e.path = it->second;
    const auto git = db.global_nets.find(it->second);
    if (git != db.global_nets.end()) {
      e.assignment_text = "global-" + git->second.category + "-source";
    } else {
      e.assignment_text = "global-net-source";
    }
    result.endpoints.push_back(std::move(e));
    result.visited_count = 1;
    return result;
  }

  const auto it = db.global_nets.find(opts.root_signal);
  if (it == db.global_nets.end()) return std::nullopt;
  result.endpoints.reserve(it->second.sinks.size());
  for (const std::string &sink : it->second.sinks) {
    EndpointRecord e;
    e.kind = EndpointKind::kExpr;
    e.path = sink;
    e.assignment_text = "global-" + it->second.category + "-sink";
    result.endpoints.push_back(std::move(e));
  }
  result.visited_count = 1;
  return result;
}

TraceRunResult RunTraceQuery(const TraceDb &db, const TraceOptions &opts) {
  if (std::optional<TraceRunResult> fast = TryRunGlobalNetFastPath(db, opts)) {
    return *fast;
  }
  TraceRunResult result;

  std::unordered_map<std::string, std::vector<std::string>> load_refs;
  std::unordered_map<std::string, std::vector<std::string>> driver_refs;
  for (const auto &[name, record] : db.signals) {
    for (const EndpointRecord &e : record.loads) {
      load_refs[EndpointPath(db, e)].push_back(name);
    }
    for (const EndpointRecord &e : record.drivers) {
      driver_refs[EndpointPath(db, e)].push_back(name);
    }
  }
  auto dedup_refs = [](std::unordered_map<std::string, std::vector<std::string>> &refs) {
    for (auto &[_, vec] : refs) {
      std::sort(vec.begin(), vec.end());
      vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
    }
  };
  dedup_refs(load_refs);
  dedup_refs(driver_refs);

  const bool is_drivers_mode = (opts.mode == "drivers");
  std::vector<EndpointRecord> logic_endpoints;
  std::vector<EndpointRecord> unresolved_ports;
  std::unordered_set<std::string> seen_logic;
  std::unordered_set<std::string> seen_ports;
  std::unordered_set<std::string> visited_signals;
  std::unordered_set<std::string> stop_once;
  bool node_cap_hit = false;

  auto record_stop = [&](const std::string &sig, const std::string &reason, const std::string &detail,
                         size_t depth) {
    const std::string key = reason + "\t" + sig + "\t" + detail;
    if (!stop_once.insert(key).second) return;
    result.stops.push_back(TraceStop{sig, reason, detail, depth});
  };

  auto endpoint_allowed = [&](const EndpointRecord &e) {
    const std::string &path = EndpointPath(db, e);
    if (!RegexMatch(opts.include_re, path)) return false;
    if (opts.exclude_re.has_value() && std::regex_search(path, *opts.exclude_re)) return false;
    return true;
  };

  std::function<void(const std::string &, size_t, size_t)> walk_signal =
      [&](const std::string &sig, size_t depth, size_t cone_depth) {
        if (depth > opts.depth_limit) {
          record_stop(sig, "depth_limit", "max-depth-reached", depth);
          return;
        }
        if (node_cap_hit) return;
        if (visited_signals.size() >= opts.max_nodes) {
          node_cap_hit = true;
          record_stop(sig, "node_limit", "max-nodes-reached", depth);
          return;
        }
        if (!visited_signals.insert(sig).second) {
          record_stop(sig, "cycle", "already-visited", depth);
          return;
        }
        if (opts.stop_at_re.has_value() && std::regex_search(sig, *opts.stop_at_re)) {
          record_stop(sig, "stop_at", "matched-stop-at-regex", depth);
          return;
        }
        const auto rec_it = db.signals.find(sig);
        if (rec_it == db.signals.end()) {
          record_stop(sig, "missing_signal", "not-in-db", depth);
          return;
        }

        const std::vector<EndpointRecord> &edges =
            is_drivers_mode ? rec_it->second.drivers : rec_it->second.loads;

        for (const EndpointRecord &e : edges) {
          const std::string &e_path = EndpointPath(db, e);
          if (sig == opts.root_signal && !EndpointMatchesSignalSelect(e, opts.signal_select)) {
            record_stop(e_path, "bit_filter", "endpoint-does-not-overlap-selected-bits", depth);
            continue;
          }
          if (e.kind != EndpointKind::kPort) {
            if (!endpoint_allowed(e)) {
              record_stop(e_path, "filtered", "expr-filtered", depth);
              continue;
            }
            const std::string key = EndpointKey(db, e);
            if (seen_logic.insert(key).second) logic_endpoints.push_back(e);
            if (cone_depth + 1 < opts.cone_level) {
              const std::vector<std::string> &next_signals =
                  is_drivers_mode ? e.rhs_signals : e.lhs_signals;
              if (next_signals.empty()) {
                bool expanded = false;
                if (opts.prefer_port_hop) {
                  const auto next_it = db.signals.find(e_path);
                  if (next_it != db.signals.end() && e_path != sig) {
                    walk_signal(e_path, depth + 1, cone_depth + 1);
                    expanded = true;
                  }
                  const auto &bridge_refs = is_drivers_mode ? load_refs : driver_refs;
                  const auto bridge_it = bridge_refs.find(e_path);
                  if (bridge_it != bridge_refs.end()) {
                    for (const std::string &next_sig : bridge_it->second) {
                      if (next_sig == sig) continue;
                      walk_signal(next_sig, depth + 1, cone_depth + 1);
                      expanded = true;
                    }
                  }
                }
                if (!expanded) record_stop(e_path, "cone_limit", "no-expandable-assignment-context", depth);
              } else {
                for (const std::string &next_sig : next_signals) {
                  if (next_sig == sig) continue;
                  walk_signal(next_sig, depth + 1, cone_depth + 1);
                }
              }
            } else if (!e.rhs_signals.empty() || !e.lhs_signals.empty()) {
              record_stop(e_path, "cone_limit", "max-cone-level-reached", depth);
            }
            continue;
          }

          bool expanded = false;
          if (e_path != sig) {
            const auto next_it = db.signals.find(e_path);
            if (next_it != db.signals.end()) {
              walk_signal(e_path, depth + 1, cone_depth);
              expanded = true;
            }
          }

          const auto &bridge_refs = is_drivers_mode ? load_refs : driver_refs;
          const auto bridge_it = bridge_refs.find(e_path);
          if (bridge_it != bridge_refs.end()) {
            for (const std::string &next_sig : bridge_it->second) {
              if (next_sig == sig) continue;
              walk_signal(next_sig, depth + 1, cone_depth);
              expanded = true;
            }
          }

          if (!expanded) {
            if (!endpoint_allowed(e)) {
              record_stop(e_path, "filtered", "port-filtered", depth);
              continue;
            }
            const std::string key = EndpointKey(db, e);
            if (seen_ports.insert(key).second) unresolved_ports.push_back(e);
          }
        }
      };

  walk_signal(opts.root_signal, 0, 0);
  result.visited_count = visited_signals.size();
  result.endpoints = logic_endpoints.empty() ? unresolved_ports : logic_endpoints;
  std::sort(result.endpoints.begin(), result.endpoints.end(),
            [&](const EndpointRecord &a, const EndpointRecord &b) {
              auto score = [&](const EndpointRecord &e) -> int {
                const std::string &path = EndpointPath(db, e);
                int s = 0;
                if (e.kind == EndpointKind::kExpr) s += 100;
                if (e.has_assignment_range || !e.assignment_text.empty()) s += 50;
                if (path == opts.root_signal) s -= 20;
                if (path.rfind(opts.root_signal, 0) == 0) s += 20;
                if (e.kind == EndpointKind::kPort) s -= 10;
                return s;
              };
              const int sa = score(a), sb = score(b);
              if (sa != sb) return sa > sb;
              const std::string &a_path = EndpointPath(db, a);
              const std::string &b_path = EndpointPath(db, b);
              if (a_path != b_path) return a_path < b_path;
              const std::string &a_file = EndpointFile(db, a);
              const std::string &b_file = EndpointFile(db, b);
              if (a_file != b_file) return a_file < b_file;
              if (a.line != b.line) return a.line < b.line;
              return a.direction < b.direction;
            });
  return result;
}

int RunTrace(int argc, char *argv[]) {
  std::optional<std::string> db_path;
  TraceOptions opts;

  for (int i = 0; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "Usage: rtl_trace trace --db <file> --mode <drivers|loads> --signal "
                   "<hier.path|hier.path[bit]|hier.path[msb:lsb]> "
                   "[--cone-level <N>] "
                   "[--prefer-port-hop] "
                   "[--depth <N>] [--max-nodes <N>] [--include <regex>] [--exclude <regex>] "
                   "[--stop-at <regex>] [--format <text|json>]\n";
      return 0;
    }
    if (arg == "--db") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --db\n";
        return 1;
      }
      db_path = argv[++i];
      continue;
    }
    if (arg == "--mode") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --mode\n";
        return 1;
      }
      opts.mode = argv[++i];
      continue;
    }
    if (arg == "--signal") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --signal\n";
        return 1;
      }
      opts.signal = argv[++i];
      continue;
    }
    if (arg == "--depth") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --depth\n";
        return 1;
      }
      opts.depth_limit = std::stoull(argv[++i]);
      continue;
    }
    if (arg == "--cone-level") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --cone-level\n";
        return 1;
      }
      const std::string val = argv[++i];
      long long parsed = 0;
      try {
        size_t pos = 0;
        parsed = std::stoll(val, &pos, 10);
        if (pos != val.size()) {
          std::cerr << "Invalid --cone-level: " << val << "\n";
          return 1;
        }
      } catch (...) {
        std::cerr << "Invalid --cone-level: " << val << "\n";
        return 1;
      }
      if (parsed < 1) {
        std::cerr << "--cone-level must be >= 1\n";
        return 1;
      }
      opts.cone_level = static_cast<size_t>(parsed);
      continue;
    }
    if (arg == "--max-nodes") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --max-nodes\n";
        return 1;
      }
      opts.max_nodes = std::stoull(argv[++i]);
      continue;
    }
    if (arg == "--prefer-port-hop") {
      opts.prefer_port_hop = true;
      continue;
    }
    if (arg == "--include") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --include\n";
        return 1;
      }
      opts.include_re = std::regex(argv[++i]);
      continue;
    }
    if (arg == "--exclude") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --exclude\n";
        return 1;
      }
      opts.exclude_re = std::regex(argv[++i]);
      continue;
    }
    if (arg == "--stop-at") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --stop-at\n";
        return 1;
      }
      opts.stop_at_re = std::regex(argv[++i]);
      continue;
    }
    if (arg == "--format") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --format\n";
        return 1;
      }
      auto fmt = ParseOutputFormat(argv[++i]);
      if (!fmt.has_value()) {
        std::cerr << "Invalid --format (expected text|json)\n";
        return 1;
      }
      opts.format = *fmt;
      continue;
    }
    std::cerr << "Unknown option: " << arg << "\n";
    return 1;
  }

  if (!db_path.has_value() || opts.mode.empty() || opts.signal.empty()) {
    std::cerr << "Missing required args: --db --mode --signal\n";
    return 1;
  }
  if (opts.mode != "drivers" && opts.mode != "loads") {
    std::cerr << "Invalid --mode: " << opts.mode << " (expected drivers|loads)\n";
    return 1;
  }
  if (!ParseSignalQuery(opts.signal, opts.root_signal, opts.signal_select)) {
    std::cerr << "Invalid --signal syntax: " << opts.signal
              << " (expected hier.path or hier.path[bit] or hier.path[msb:lsb])\n";
    return 1;
  }

  TraceDb db;
  if (!LoadDb(*db_path, db)) {
    std::cerr << "Failed to read DB: " << *db_path << "\n";
    return 1;
  }
  if (db.signals.find(opts.root_signal) == db.signals.end()) {
    std::cerr << "Signal not found: " << opts.root_signal << "\n";
    for (const std::string &s : TopSuggestions(db, opts.root_signal, 5)) {
      std::cerr << "  suggestion: " << s << "\n";
    }
    return 2;
  }

  TraceRunResult result = RunTraceQuery(db, opts);
  MaterializeAssignmentTexts(db, result);
  if (opts.format == OutputFormat::kJson) {
    PrintTraceJson(db, opts, result);
  } else {
    PrintTraceText(db, opts, result);
  }
  return 0;
}

std::vector<std::string> HierRoots(const TraceDb &db) {
  std::vector<std::string> roots;
  for (const auto &[path, _] : db.hierarchy) {
    const std::string parent = ParentPath(path);
    if (parent.empty() || db.hierarchy.find(parent) == db.hierarchy.end()) {
      roots.push_back(path);
    }
  }
  std::sort(roots.begin(), roots.end());
  roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
  return roots;
}

std::vector<std::string> TopHierarchySuggestions(const TraceDb &db, const std::string &needle,
                                                 size_t limit) {
  std::vector<std::pair<size_t, std::string>> scored;
  scored.reserve(db.hierarchy.size());
  for (const auto &[name, _] : db.hierarchy) {
    scored.push_back({EditDistance(name, needle), name});
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

HierRunResult RunHierQuery(const TraceDb &db, const HierOptions &opts) {
  HierRunResult result;
  result.root = opts.root;
  result.depth_limit = opts.depth_limit;
  if (db.hierarchy.empty()) {
    result.stops.push_back("empty-hierarchy");
    return result;
  }

  std::unordered_set<std::string> active;
  bool truncated = false;
  std::vector<std::string> stops;

  std::function<std::optional<HierTreeNode>(const std::string &, size_t)> build =
      [&](const std::string &path, size_t depth) -> std::optional<HierTreeNode> {
    if (result.node_count >= opts.max_nodes) {
      truncated = true;
      return std::nullopt;
    }
    if (!active.insert(path).second) {
      stops.push_back("cycle@" + path);
      return std::nullopt;
    }
    const auto it = db.hierarchy.find(path);
    if (it == db.hierarchy.end()) {
      active.erase(path);
      return std::nullopt;
    }
    HierTreeNode node;
    node.path = path;
    node.module = it->second.module;
    result.node_count++;

    if (depth >= opts.depth_limit) {
      if (!it->second.children.empty()) stops.push_back("depth_limit@" + path);
      active.erase(path);
      return node;
    }

    for (const std::string &child : it->second.children) {
      if (result.node_count >= opts.max_nodes) {
        truncated = true;
        stops.push_back("max_nodes@" + path);
        break;
      }
      std::optional<HierTreeNode> child_node = build(child, depth + 1);
      if (child_node.has_value()) node.children.push_back(std::move(*child_node));
    }
    active.erase(path);
    return node;
  };

  result.tree = build(result.root, 0);
  result.truncated = truncated;
  std::sort(stops.begin(), stops.end());
  stops.erase(std::unique(stops.begin(), stops.end()), stops.end());
  result.stops = std::move(stops);
  return result;
}

void PrintHierTreeText(const HierTreeNode &node, size_t indent) {
  std::string lead(indent * 2, ' ');
  std::cout << lead << LeafName(node.path);
  if (!node.module.empty()) std::cout << " (module=" << node.module << ")";
  std::cout << "\n";
  for (const auto &child : node.children) {
    PrintHierTreeText(child, indent + 1);
  }
}

void PrintHierText(const HierRunResult &result) {
  std::cout << "root: " << result.root << "\n";
  std::cout << "depth: " << result.depth_limit << "\n";
  std::cout << "nodes: " << result.node_count << "\n";
  if (result.truncated) std::cout << "truncated: true\n";
  if (result.tree.has_value()) {
    std::cout << "\n";
    PrintHierTreeText(*result.tree, 0);
  }
  if (!result.stops.empty()) {
    std::cout << "stops: " << result.stops.size() << "\n";
    for (const std::string &s : result.stops) {
      std::cout << "  stop  " << s << "\n";
    }
  }
}

void PrintHierTreeJson(const HierTreeNode &node) {
  std::cout << "{\"path\":\"" << JsonEscape(node.path) << "\",\"module\":\"" << JsonEscape(node.module)
            << "\",\"children\":[";
  for (size_t i = 0; i < node.children.size(); ++i) {
    if (i) std::cout << ",";
    PrintHierTreeJson(node.children[i]);
  }
  std::cout << "]}";
}

void PrintHierJson(const HierRunResult &result) {
  std::cout << "{\"root\":\"" << JsonEscape(result.root) << "\",\"depth_limit\":" << result.depth_limit
            << ",\"node_count\":" << result.node_count
            << ",\"truncated\":" << (result.truncated ? "true" : "false") << ",\"tree\":";
  if (result.tree.has_value()) {
    PrintHierTreeJson(*result.tree);
  } else {
    std::cout << "null";
  }
  std::cout << ",\"stops\":[";
  for (size_t i = 0; i < result.stops.size(); ++i) {
    if (i) std::cout << ",";
    std::cout << "\"" << JsonEscape(result.stops[i]) << "\"";
  }
  std::cout << "]}\n";
}

int RunHier(int argc, char *argv[]) {
  std::optional<std::string> db_path;
  HierOptions opts;
  opts.root.clear();

  for (int i = 0; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "Usage: rtl_trace hier --db <file> [--root <hier.path>] [--depth <N>] "
                   "[--max-nodes <N>] [--format <text|json>]\n";
      return 0;
    }
    if (arg == "--db") {
      if (i + 1 >= argc) return std::cerr << "Missing value for --db\n", 1;
      db_path = argv[++i];
      continue;
    }
    if (arg == "--root") {
      if (i + 1 >= argc) return std::cerr << "Missing value for --root\n", 1;
      opts.root = argv[++i];
      continue;
    }
    if (arg == "--depth") {
      if (i + 1 >= argc) return std::cerr << "Missing value for --depth\n", 1;
      opts.depth_limit = std::stoull(argv[++i]);
      continue;
    }
    if (arg == "--max-nodes") {
      if (i + 1 >= argc) return std::cerr << "Missing value for --max-nodes\n", 1;
      opts.max_nodes = std::stoull(argv[++i]);
      continue;
    }
    if (arg == "--format") {
      if (i + 1 >= argc) return std::cerr << "Missing value for --format\n", 1;
      auto fmt = ParseOutputFormat(argv[++i]);
      if (!fmt.has_value()) return std::cerr << "Invalid --format (expected text|json)\n", 1;
      opts.format = *fmt;
      continue;
    }
    return std::cerr << "Unknown option: " << arg << "\n", 1;
  }

  if (!db_path.has_value()) return std::cerr << "Missing required args: --db\n", 1;
  TraceDb db;
  if (!LoadDb(*db_path, db)) return std::cerr << "Failed to read DB: " << *db_path << "\n", 1;
  BuildHierarchyFromSignals(db);

  if (db.hierarchy.empty()) return std::cerr << "No hierarchy data in DB\n", 1;
  if (opts.root.empty()) {
    const auto roots = HierRoots(db);
    if (roots.empty()) return std::cerr << "No hierarchy roots found in DB\n", 1;
    opts.root = roots.front();
  }
  if (db.hierarchy.find(opts.root) == db.hierarchy.end()) {
    std::cerr << "Root not found: " << opts.root << "\n";
    for (const std::string &s : TopHierarchySuggestions(db, opts.root, 5)) {
      std::cerr << "  suggestion: " << s << "\n";
    }
    return 2;
  }

  HierRunResult result = RunHierQuery(db, opts);
  if (opts.format == OutputFormat::kJson) {
    PrintHierJson(result);
  } else {
    PrintHierText(result);
  }
  return 0;
}

int RunFind(int argc, char *argv[]) {
  std::optional<std::string> db_path;
  std::optional<std::string> query;
  bool regex_mode = false;
  size_t limit = 20;
  OutputFormat format = OutputFormat::kText;

  for (int i = 0; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "Usage: rtl_trace find --db <file> --query <text|regex> [--regex] [--limit <N>] "
                   "[--format <text|json>]\n";
      return 0;
    }
    if (arg == "--db") {
      if (i + 1 >= argc) return std::cerr << "Missing value for --db\n", 1;
      db_path = argv[++i];
      continue;
    }
    if (arg == "--query") {
      if (i + 1 >= argc) return std::cerr << "Missing value for --query\n", 1;
      query = argv[++i];
      continue;
    }
    if (arg == "--regex") {
      regex_mode = true;
      continue;
    }
    if (arg == "--limit") {
      if (i + 1 >= argc) return std::cerr << "Missing value for --limit\n", 1;
      limit = std::stoull(argv[++i]);
      continue;
    }
    if (arg == "--format") {
      if (i + 1 >= argc) return std::cerr << "Missing value for --format\n", 1;
      auto fmt = ParseOutputFormat(argv[++i]);
      if (!fmt.has_value()) return std::cerr << "Invalid --format (expected text|json)\n", 1;
      format = *fmt;
      continue;
    }
    return std::cerr << "Unknown option: " << arg << "\n", 1;
  }

  if (!db_path.has_value() || !query.has_value()) {
    std::cerr << "Missing required args: --db --query\n";
    return 1;
  }
  TraceDb db;
  if (!LoadDb(*db_path, db)) {
    std::cerr << "Failed to read DB: " << *db_path << "\n";
    return 1;
  }

  std::vector<std::string> matches;
  std::optional<std::regex> re;
  if (regex_mode) re = std::regex(*query);
  for (const auto &[name, _] : db.signals) {
    bool ok = false;
    if (regex_mode) {
      ok = std::regex_search(name, *re);
    } else {
      ok = (name.find(*query) != std::string::npos);
    }
    if (ok) matches.push_back(name);
  }
  std::sort(matches.begin(), matches.end());
  if (matches.size() > limit) matches.resize(limit);

  std::vector<std::string> suggestions;
  if (matches.empty()) suggestions = TopSuggestions(db, *query, limit);

  if (format == OutputFormat::kJson) {
    std::cout << "{\"query\":\"" << JsonEscape(*query) << "\",\"regex\":"
              << (regex_mode ? "true" : "false") << ",\"count\":" << matches.size() << ",\"matches\":[";
    for (size_t i = 0; i < matches.size(); ++i) {
      if (i) std::cout << ",";
      std::cout << "\"" << JsonEscape(matches[i]) << "\"";
    }
    std::cout << "],\"suggestions\":[";
    for (size_t i = 0; i < suggestions.size(); ++i) {
      if (i) std::cout << ",";
      std::cout << "\"" << JsonEscape(suggestions[i]) << "\"";
    }
    std::cout << "]}\n";
  } else {
    std::cout << "query: " << *query << "\n";
    std::cout << "regex: " << (regex_mode ? "true" : "false") << "\n";
    std::cout << "count: " << matches.size() << "\n";
    for (const std::string &m : matches) {
      std::cout << "signal " << m << "\n";
    }
    if (matches.empty() && !suggestions.empty()) {
      std::cout << "suggestions:\n";
      for (const std::string &s : suggestions) {
        std::cout << "  " << s << "\n";
      }
    }
  }
  return matches.empty() ? 2 : 0;
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc < 2) {
    PrintGeneralHelp();
    return 1;
  }

  const std::string subcmd = argv[1];
  if (subcmd == "-h" || subcmd == "--help") {
    PrintGeneralHelp();
    return 0;
  }
  if (subcmd == "compile") {
    return RunCompile(argc - 2, argv + 2);
  }
  if (subcmd == "trace") {
    return RunTrace(argc - 2, argv + 2);
  }
  if (subcmd == "hier") {
    return RunHier(argc - 2, argv + 2);
  }
  if (subcmd == "find") {
    return RunFind(argc - 2, argv + 2);
  }

  std::cerr << "Unknown subcommand: " << subcmd << "\n";
  PrintGeneralHelp();
  return 1;
}
