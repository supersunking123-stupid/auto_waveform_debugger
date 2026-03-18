#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/EvalContext.h"
#include "slang/ast/Expression.h"
#include "slang/ast/Scope.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/expressions/AssignmentExpressions.h"
#include "slang/ast/expressions/LiteralExpressions.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/driver/Driver.h"
#include "slang/text/SourceManager.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
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
};

using TraceResult = std::variant<const slang::ast::PortSymbol *, ExprTraceResult>;

enum class EndpointKind { kPort, kExpr };

struct EndpointRecord {
  EndpointKind kind = EndpointKind::kExpr;
  std::string path;
  std::string file;
  int line = 0;
  int col = 0;
  std::string direction;
  std::string assignment_text;
  std::string bit_map;
  bool bit_map_approximate = false;
  std::vector<std::string> lhs_signals;
  std::vector<std::string> rhs_signals;
};

struct SignalRecord {
  std::vector<EndpointRecord> drivers;
  std::vector<EndpointRecord> loads;
};

struct TraceDb {
  std::unordered_map<std::string, SignalRecord> signals;
};

enum class OutputFormat { kText, kJson };

struct TraceOptions {
  std::string mode;
  std::string signal;
  std::string root_signal;
  std::optional<std::pair<int32_t, int32_t>> signal_select;
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

std::vector<std::string> CollectLhsSignalsFromStatement(const slang::ast::Statement &stmt);

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
class TraceFinder : public slang::ast::ASTVisitor<TraceFinder<DRIVERS>, true, true> {
 public:
  TraceFinder(const slang::ast::Symbol *sym, std::vector<TraceResult> &out,
              std::unordered_set<const slang::ast::Symbol *> &visited)
      : sym_(sym), out_(out), visited_(visited) {}

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
    if (&port == sym_ || (port.internalSymbol != nullptr && port.internalSymbol == sym_)) {
      if constexpr (DRIVERS) {
        if (port.direction == slang::ast::ArgumentDirection::In &&
            FollowThroughParentConnection(port)) {
          return;
        }
      } else {
        if (port.direction == slang::ast::ArgumentDirection::Out &&
            FollowThroughParentConnection(port)) {
          return;
        }
      }
      out_.push_back(&port);
    }
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
    std::vector<std::string> context_lhs = CollectLhsSignalsFromStatement(stmt.ifTrue);
    if (stmt.ifFalse != nullptr) {
      std::vector<std::string> else_lhs = CollectLhsSignalsFromStatement(*stmt.ifFalse);
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
      std::vector<std::string> item_lhs = CollectLhsSignalsFromStatement(*item.stmt);
      context_lhs.insert(context_lhs.end(), item_lhs.begin(), item_lhs.end());
    }
    if (stmt.defaultCase != nullptr) {
      std::vector<std::string> default_lhs = CollectLhsSignalsFromStatement(*stmt.defaultCase);
      context_lhs.insert(context_lhs.end(), default_lhs.begin(), default_lhs.end());
    }
    std::sort(context_lhs.begin(), context_lhs.end());
    context_lhs.erase(std::unique(context_lhs.begin(), context_lhs.end()), context_lhs.end());

    condition_lhs_stack_.push_back(std::move(context_lhs));
    const slang::ast::Expression *saved_condition = current_condition_expr_;
    current_condition_expr_ = &stmt.expr;
    stmt.expr.visit(*this);
    current_condition_expr_ = saved_condition;

    for (const auto &item : stmt.items) {
      item.stmt->visit(*this);
    }
    if (stmt.defaultCase != nullptr) stmt.defaultCase->visit(*this);
    condition_lhs_stack_.pop_back();
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
    if (&nve.symbol != sym_) return;
    if (checking_instance_port_expression_ && FollowIntoChildInstance()) return;
    if constexpr (DRIVERS) {
      if (checking_instance_port_expression_ || checking_lhs_) {
        ExprTraceResult result;
        result.expr = &nve;
        if (checking_lhs_) result.assignment = current_assignment_;
        result.selectors = selector_stack_;
        out_.push_back(result);
      }
    } else {
      if (!(checking_lhs_ && selector_depth_ == 0)) {
        ExprTraceResult result;
        result.expr = &nve;
        result.assignment = current_assignment_;
        result.selectors = selector_stack_;
        if (current_assignment_ == nullptr && current_condition_expr_ != nullptr &&
            !condition_lhs_stack_.empty()) {
          result.context_lhs_signals = condition_lhs_stack_.back();
        }
        out_.push_back(result);
      }
    }
  }

  void handle(const slang::ast::UninstantiatedDefSymbol &uninst) {}

 private:
  class PortExprCollector : public slang::ast::ASTVisitor<PortExprCollector, true, true> {
   public:
    PortExprCollector(std::vector<TraceResult> &out,
                      std::unordered_set<const slang::ast::Symbol *> &visited)
        : out_(out), visited_(visited) {}

    void handle(const slang::ast::NamedValueExpression &nve) {
      if (!IsTraceable(&nve.symbol)) return;
      if (!visited_.insert(&nve.symbol).second) return;
      ExprTraceResult result;
      result.expr = &nve;
      result.assignment = nullptr;
      out_.push_back(result);
    }

   private:
    std::vector<TraceResult> &out_;
    std::unordered_set<const slang::ast::Symbol *> &visited_;
  };

  bool FollowThroughParentConnection(const slang::ast::PortSymbol &port) {
    const slang::ast::Symbol *context = sym_;
    if (const auto *sym_port = sym_->as_if<slang::ast::PortSymbol>()) {
      if (sym_port->internalSymbol != nullptr) {
        context = sym_port->internalSymbol;
      }
    }
    const slang::ast::InstanceSymbol *inst = GetContainingInstanceSymbol(context);
    if (inst == nullptr) return false;

    const size_t before = out_.size();
    for (const slang::ast::PortConnection *conn : inst->getPortConnections()) {
      const auto *conn_port = conn->port.template as_if<slang::ast::PortSymbol>();
      if (conn_port != &port) continue;
      const slang::ast::Expression *expr = conn->getExpression();
      if (expr == nullptr) continue;
      PortExprCollector collector(out_, visited_);
      expr->visit(collector);
      break;
    }
    return out_.size() > before;
  }

  bool FollowIntoChildInstance() {
    if (active_instance_ == nullptr || active_port_ == nullptr ||
        active_port_->internalSymbol == nullptr) {
      return false;
    }
    const slang::ast::Symbol *internal = active_port_->internalSymbol;
    if (!visited_.insert(internal).second) return false;
    const size_t before = out_.size();
    TraceFinder<DRIVERS> nested(internal, out_, visited_);
    active_instance_->body.visit(nested);
    return out_.size() > before;
  }

  const slang::ast::Symbol *sym_;
  std::vector<TraceResult> &out_;
  std::unordered_set<const slang::ast::Symbol *> &visited_;
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
};

std::vector<TraceResult> GetDrivers(const slang::ast::Symbol *sym) {
  std::vector<TraceResult> out;
  const slang::ast::InstanceBodySymbol *body = GetContainingInstance(sym);
  if (body == nullptr) return out;
  std::unordered_set<const slang::ast::Symbol *> visited;
  visited.insert(sym);
  TraceFinder</*DRIVERS*/ true> finder(sym, out, visited);
  body->visit(finder);
  return out;
}

std::vector<TraceResult> GetLoads(const slang::ast::Symbol *sym) {
  std::vector<TraceResult> out;
  const slang::ast::InstanceBodySymbol *body = GetContainingInstance(sym);
  if (body == nullptr) return out;
  std::unordered_set<const slang::ast::Symbol *> visited;
  visited.insert(sym);
  TraceFinder</*DRIVERS*/ false> finder(sym, out, visited);
  body->visit(finder);
  return out;
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

EndpointRecord ResolveTraceResult(const TraceResult &r, const slang::SourceManager &sm) {
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
          rec.col = sm.getColumnNumber(loc);
        } else {
          rec.kind = EndpointKind::kExpr;
          rec.path = item.expr->symbol.getHierarchicalPath();
          const auto loc = item.expr->sourceRange.start();
          rec.file = std::string(sm.getFileName(loc));
          rec.line = sm.getLineNumber(loc);
          rec.col = sm.getColumnNumber(loc);
          auto bit_desc = DescribeBitSelectors(item.selectors, sm, item.expr->symbol);
          rec.bit_map = std::move(bit_desc.first);
          rec.bit_map_approximate = bit_desc.second;
          if (item.assignment != nullptr) {
            rec.assignment_text = GetSourceText(item.assignment->sourceRange, sm);
            rec.lhs_signals = CollectLhsSignals(item.assignment);
            rec.rhs_signals = CollectRhsSignals(item.assignment);
          } else if (!item.context_lhs_signals.empty()) {
            rec.lhs_signals = item.context_lhs_signals;
          }
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

bool SaveDb(const std::string &db_path, const TraceDb &db) {
  std::ofstream out(db_path);
  if (!out.is_open()) return false;

  out << "RTL_TRACE_DB_V4\n";
  std::vector<std::string> signal_names;
  signal_names.reserve(db.signals.size());
  for (const auto &[name, _] : db.signals) {
    signal_names.push_back(name);
  }
  std::sort(signal_names.begin(), signal_names.end());

  auto write_endpoint = [&](char kind, const EndpointRecord &e) {
    out << kind << '\t' << (e.kind == EndpointKind::kPort ? 'P' : 'E') << '\t'
        << EscapeField(e.path) << '\t' << EscapeField(e.file) << '\t' << e.line << '\t' << e.col
        << '\t' << EscapeField(e.direction) << '\t' << EscapeField(e.bit_map) << '\t'
        << (e.bit_map_approximate ? "1" : "0");
    if (!e.assignment_text.empty() || !e.lhs_signals.empty() || !e.rhs_signals.empty()) {
      std::string lhs_joined;
      for (size_t i = 0; i < e.lhs_signals.size(); ++i) {
        if (i != 0) lhs_joined += '\n';
        lhs_joined += e.lhs_signals[i];
      }
      std::string rhs_joined;
      for (size_t i = 0; i < e.rhs_signals.size(); ++i) {
        if (i != 0) rhs_joined += '\n';
        rhs_joined += e.rhs_signals[i];
      }
      out << '\t' << EscapeField(e.assignment_text) << '\t' << EscapeField(lhs_joined) << '\t'
          << EscapeField(rhs_joined);
    }
    out << '\n';
  };

  for (const std::string &name : signal_names) {
    const auto it = db.signals.find(name);
    if (it == db.signals.end()) continue;
    out << "S\t" << EscapeField(name) << '\n';
    for (const EndpointRecord &e : it->second.drivers)
      write_endpoint('D', e);
    for (const EndpointRecord &e : it->second.loads)
      write_endpoint('L', e);
    out << "X\n";
  }
  return true;
}

bool LoadDb(const std::string &db_path, TraceDb &db) {
  std::ifstream in(db_path);
  if (!in.is_open()) return false;

  std::string line;
  if (!std::getline(in, line)) return false;
  if (line != "RTL_TRACE_DB_V1" && line != "RTL_TRACE_DB_V2" && line != "RTL_TRACE_DB_V3" &&
      line != "RTL_TRACE_DB_V4")
    return false;
  const bool has_lhs_column = (line == "RTL_TRACE_DB_V3" || line == "RTL_TRACE_DB_V4");
  const bool has_bit_column = (line == "RTL_TRACE_DB_V4");

  SignalRecord *current = nullptr;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    const std::vector<std::string> fields = SplitEscapedTsv(line);
    if (fields.empty()) continue;
    if (fields[0] == "S") {
      if (fields.size() != 2) return false;
      auto [it, inserted] = db.signals.emplace(fields[1], SignalRecord{});
      current = &it->second;
      continue;
    }
    if (fields[0] == "X") {
      current = nullptr;
      continue;
    }
    if ((fields[0] == "D" || fields[0] == "L") && current != nullptr) {
      if (fields.size() < 7) return false;
      EndpointRecord e;
      e.kind = fields[1] == "P" ? EndpointKind::kPort : EndpointKind::kExpr;
      e.path = fields[2];
      e.file = fields[3];
      e.line = std::stoi(fields[4]);
      e.col = std::stoi(fields[5]);
      e.direction = fields[6];
      size_t next_field = 7;
      if (has_bit_column) {
        if (fields.size() > next_field) e.bit_map = fields[next_field];
        if (fields.size() > next_field + 1) e.bit_map_approximate = (fields[next_field + 1] == "1");
        next_field += 2;
      }
      if (fields.size() > next_field) {
        e.assignment_text = fields[next_field];
      }
      if (has_lhs_column && fields.size() > next_field + 1 && !fields[next_field + 1].empty()) {
        std::string lhs_field = fields[next_field + 1];
        size_t start = 0;
        while (start <= lhs_field.size()) {
          const size_t pos = lhs_field.find('\n', start);
          if (pos == std::string::npos) {
            e.lhs_signals.push_back(lhs_field.substr(start));
            break;
          }
          e.lhs_signals.push_back(lhs_field.substr(start, pos - start));
          start = pos + 1;
        }
      }
      const size_t rhs_index = has_lhs_column ? (next_field + 2) : (next_field + 1);
      if (fields.size() > rhs_index && !fields[rhs_index].empty()) {
        std::string rhs_field = fields[rhs_index];
        size_t start = 0;
        while (start <= rhs_field.size()) {
          const size_t pos = rhs_field.find('\n', start);
          if (pos == std::string::npos) {
            e.rhs_signals.push_back(rhs_field.substr(start));
            break;
          }
          e.rhs_signals.push_back(rhs_field.substr(start, pos - start));
          start = pos + 1;
        }
      }
      if (fields[0] == "D") {
        current->drivers.push_back(std::move(e));
      } else {
        current->loads.push_back(std::move(e));
      }
      continue;
    }
    return false;
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

std::string EndpointKey(const EndpointRecord &e) {
  return std::to_string(static_cast<int>(e.kind)) + "\t" + e.path + "\t" + e.file + "\t" +
         std::to_string(e.line) + "\t" + std::to_string(e.col) + "\t" + e.direction + "\t" +
         e.assignment_text + "\t" + e.bit_map + "\t" + (e.bit_map_approximate ? "1" : "0");
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
  std::cout << "  rtl_trace compile [--db <file>] [--incremental] [slang source args...]\n";
  std::cout << "  rtl_trace trace --db <file> --mode <drivers|loads> --signal <hier.path> "
               "[--depth <N>] [--max-nodes <N>] [--include <regex>] [--exclude <regex>] "
               "[--stop-at <regex>] [--format <text|json>]\n";
  std::cout << "  rtl_trace find --db <file> --query <text|regex> [--regex] [--limit <N>] "
               "[--format <text|json>]\n";
}

int RunCompile(int argc, char *argv[]) {
  std::string db_path = "rtl_trace.db";
  bool incremental = false;
  std::vector<std::string> passthrough_args;

  for (int i = 0; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "Usage: rtl_trace compile [--db <file>] [--incremental] [slang source args...]\n";
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
    passthrough_args.push_back(arg);
  }

  const std::filesystem::path db_path_fs = std::filesystem::path(db_path);
  const std::filesystem::path meta_path = db_path_fs.string() + ".meta";
  const std::string new_fingerprint = ComputeCompileFingerprint(passthrough_args);
  if (incremental && std::filesystem::exists(db_path_fs) && std::filesystem::exists(meta_path)) {
    std::ifstream meta_in(meta_path);
    std::string old_fingerprint((std::istreambuf_iterator<char>(meta_in)),
                                std::istreambuf_iterator<char>());
    if (old_fingerprint == new_fingerprint) {
      std::cout << "db: " << db_path << "\n";
      std::cout << "signals: incremental-cache-hit\n";
      return 0;
    }
  }

  std::vector<std::string> driver_args;
  driver_args.reserve(passthrough_args.size() + 3);
  driver_args.emplace_back("rtl_trace_compile");
  if (!HasTimescaleArg(passthrough_args)) {
    driver_args.emplace_back("--timescale");
    driver_args.emplace_back("1ns/1ps");
  }
  for (const std::string &arg : passthrough_args) {
    driver_args.push_back(arg);
  }

  std::vector<char *> driver_argv;
  driver_argv.reserve(driver_args.size());
  for (std::string &arg : driver_args) {
    driver_argv.push_back(arg.data());
  }

  slang::driver::Driver driver;
  driver.addStandardArgs();
  if (!driver.parseCommandLine(static_cast<int>(driver_argv.size()), driver_argv.data())) return 1;
  if (!driver.processOptions()) return 1;
  if (!driver.parseAllSources()) return 1;
  std::unique_ptr<slang::ast::Compilation> compilation = driver.createCompilation();
  driver.reportCompilation(*compilation, /*quiet*/ true);
  if (!driver.reportDiagnostics(/*quiet*/ true)) return 1;

  const slang::ast::RootSymbol &root = compilation->getRoot();
  const slang::SourceManager &sm = *compilation->getSourceManager();

  std::unordered_map<std::string, const slang::ast::Symbol *> symbols;
  CollectTraceableSymbols(root, symbols);

  std::vector<std::string> keys;
  keys.reserve(symbols.size());
  for (const auto &[k, _] : symbols)
    keys.push_back(k);
  std::sort(keys.begin(), keys.end());

  TraceDb db;
  db.signals.reserve(keys.size());
  for (const std::string &path : keys) {
    const slang::ast::Symbol *sym = symbols[path];
    if (!IsTraceable(sym)) continue;
    SignalRecord rec;
    for (const TraceResult &r : GetDrivers(sym)) {
      rec.drivers.push_back(ResolveTraceResult(r, sm));
    }
    for (const TraceResult &r : GetLoads(sym)) {
      rec.loads.push_back(ResolveTraceResult(r, sm));
    }
    db.signals.emplace(path, std::move(rec));
  }

  if (!SaveDb(db_path, db)) {
    std::cerr << "Failed to write DB: " << db_path << "\n";
    return 1;
  }
  std::ofstream meta_out(meta_path);
  if (meta_out.is_open()) meta_out << new_fingerprint;
  std::cout << "db: " << db_path << "\n";
  std::cout << "signals: " << db.signals.size() << "\n";
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

void PrintTraceText(const TraceOptions &opts, const TraceRunResult &result) {
  std::cout << "target: " << opts.signal << "\n";
  std::cout << "mode: " << opts.mode << "\n";
  std::cout << "count: " << result.endpoints.size() << "\n";
  std::cout << "visited: " << result.visited_count << "\n";
  for (const EndpointRecord &e : result.endpoints) {
    if (e.kind == EndpointKind::kPort) {
      std::cout << "port  " << e.path << " (" << e.direction << ") @ " << e.file << ":" << e.line
                << ":" << e.col << "\n";
    } else {
      std::cout << "expr  " << e.path << " @ " << e.file << ":" << e.line << ":" << e.col << "\n";
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

void PrintTraceJson(const TraceOptions &opts, const TraceRunResult &result) {
  std::cout << "{";
  std::cout << "\"target\":\"" << JsonEscape(opts.signal) << "\",";
  std::cout << "\"mode\":\"" << JsonEscape(opts.mode) << "\",";
  std::cout << "\"summary\":{\"count\":" << result.endpoints.size() << ",\"visited\":"
            << result.visited_count << ",\"stops\":" << result.stops.size() << "},";
  std::cout << "\"endpoints\":[";
  for (size_t i = 0; i < result.endpoints.size(); ++i) {
    const EndpointRecord &e = result.endpoints[i];
    if (i) std::cout << ",";
    std::cout << "{"
              << "\"kind\":\"" << (e.kind == EndpointKind::kPort ? "port" : "expr") << "\","
              << "\"path\":\"" << JsonEscape(e.path) << "\","
              << "\"file\":\"" << JsonEscape(e.file) << "\","
              << "\"line\":" << e.line << ",\"col\":" << e.col << ","
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

TraceRunResult RunTraceQuery(const TraceDb &db, const TraceOptions &opts) {
  TraceRunResult result;

  std::unordered_map<std::string, std::vector<std::string>> load_refs;
  std::unordered_map<std::string, std::vector<std::string>> driver_refs;
  for (const auto &[name, record] : db.signals) {
    for (const EndpointRecord &e : record.loads) {
      load_refs[e.path].push_back(name);
    }
    for (const EndpointRecord &e : record.drivers) {
      driver_refs[e.path].push_back(name);
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
    if (!RegexMatch(opts.include_re, e.path)) return false;
    if (opts.exclude_re.has_value() && std::regex_search(e.path, *opts.exclude_re)) return false;
    return true;
  };

  std::function<void(const std::string &, size_t)> walk_signal =
      [&](const std::string &sig, size_t depth) {
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
          if (sig == opts.root_signal && !EndpointMatchesSignalSelect(e, opts.signal_select)) {
            record_stop(e.path, "bit_filter", "endpoint-does-not-overlap-selected-bits", depth);
            continue;
          }
          if (e.kind != EndpointKind::kPort) {
            if (!endpoint_allowed(e)) {
              record_stop(e.path, "filtered", "expr-filtered", depth);
              continue;
            }
            const std::string key = EndpointKey(e);
            if (seen_logic.insert(key).second) logic_endpoints.push_back(e);
            continue;
          }

          bool expanded = false;
          if (e.path != sig) {
            const auto next_it = db.signals.find(e.path);
            if (next_it != db.signals.end()) {
              walk_signal(e.path, depth + 1);
              expanded = true;
            }
          }

          const auto &bridge_refs = is_drivers_mode ? load_refs : driver_refs;
          const auto bridge_it = bridge_refs.find(e.path);
          if (bridge_it != bridge_refs.end()) {
            for (const std::string &next_sig : bridge_it->second) {
              if (next_sig == sig) continue;
              walk_signal(next_sig, depth + 1);
              expanded = true;
            }
          }

          if (!expanded) {
            if (!endpoint_allowed(e)) {
              record_stop(e.path, "filtered", "port-filtered", depth);
              continue;
            }
            const std::string key = EndpointKey(e);
            if (seen_ports.insert(key).second) unresolved_ports.push_back(e);
          }
        }
      };

  walk_signal(opts.root_signal, 0);
  result.visited_count = visited_signals.size();
  result.endpoints = logic_endpoints.empty() ? unresolved_ports : logic_endpoints;
  std::sort(result.endpoints.begin(), result.endpoints.end(),
            [&](const EndpointRecord &a, const EndpointRecord &b) {
              auto score = [&](const EndpointRecord &e) -> int {
                int s = 0;
                if (e.kind == EndpointKind::kExpr) s += 100;
                if (!e.assignment_text.empty()) s += 50;
                if (e.path == opts.root_signal) s -= 20;
                if (e.path.rfind(opts.root_signal, 0) == 0) s += 20;
                if (e.kind == EndpointKind::kPort) s -= 10;
                return s;
              };
              const int sa = score(a), sb = score(b);
              if (sa != sb) return sa > sb;
              if (a.path != b.path) return a.path < b.path;
              if (a.file != b.file) return a.file < b.file;
              if (a.line != b.line) return a.line < b.line;
              return a.col < b.col;
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
    if (arg == "--max-nodes") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --max-nodes\n";
        return 1;
      }
      opts.max_nodes = std::stoull(argv[++i]);
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
  if (opts.format == OutputFormat::kJson) {
    PrintTraceJson(opts, result);
  } else {
    PrintTraceText(opts, result);
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
  if (subcmd == "find") {
    return RunFind(argc - 2, argv + 2);
  }

  std::cerr << "Unknown subcommand: " << subcmd << "\n";
  PrintGeneralHelp();
  return 1;
}
