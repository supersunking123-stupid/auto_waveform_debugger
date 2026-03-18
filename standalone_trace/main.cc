#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/Expression.h"
#include "slang/ast/Scope.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/expressions/AssignmentExpressions.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/driver/Driver.h"
#include "slang/text/SourceManager.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
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

  void handle(const slang::ast::RangeSelectExpression &expr) {
    expr.value().visit(*this);
    if constexpr (!DRIVERS) {
      selector_depth_++;
      expr.left().visit(*this);
      expr.right().visit(*this);
      selector_depth_--;
    }
  }

  void handle(const slang::ast::ElementSelectExpression &expr) {
    expr.value().visit(*this);
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
        out_.push_back(result);
      }
    } else {
      if (!(checking_lhs_ && selector_depth_ == 0)) {
        ExprTraceResult result;
        result.expr = &nve;
        result.assignment = current_assignment_;
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
          if (item.assignment != nullptr) {
            rec.assignment_text = GetSourceText(item.assignment->sourceRange, sm);
            rec.lhs_signals = CollectLhsSignals(item.assignment);
            rec.rhs_signals = CollectRhsSignals(item.assignment);
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

  out << "RTL_TRACE_DB_V3\n";
  std::vector<std::string> signal_names;
  signal_names.reserve(db.signals.size());
  for (const auto &[name, _] : db.signals) {
    signal_names.push_back(name);
  }
  std::sort(signal_names.begin(), signal_names.end());

  auto write_endpoint = [&](char kind, const EndpointRecord &e) {
    out << kind << '\t' << (e.kind == EndpointKind::kPort ? 'P' : 'E') << '\t'
        << EscapeField(e.path) << '\t' << EscapeField(e.file) << '\t' << e.line << '\t' << e.col
        << '\t' << EscapeField(e.direction);
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
  if (line != "RTL_TRACE_DB_V1" && line != "RTL_TRACE_DB_V2" && line != "RTL_TRACE_DB_V3")
    return false;
  const bool has_lhs_column = (line == "RTL_TRACE_DB_V3");

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
      if (fields.size() >= 8) {
        e.assignment_text = fields[7];
      }
      if (has_lhs_column && fields.size() >= 9 && !fields[8].empty()) {
        std::string lhs_field = fields[8];
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
      const size_t rhs_index = has_lhs_column ? 9 : 8;
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

void PrintGeneralHelp() {
  std::cout << "rtl_trace: standalone RTL driver/load tracer\n\n";
  std::cout << "Usage:\n";
  std::cout << "  rtl_trace compile [--db <file>] [slang source args...]\n";
  std::cout << "  rtl_trace trace --db <file> --mode <drivers|loads> --signal <hier.path>\n";
}

int RunCompile(int argc, char *argv[]) {
  std::string db_path = "rtl_trace.db";
  std::vector<std::string> passthrough_args;

  for (int i = 0; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "Usage: rtl_trace compile [--db <file>] [slang source args...]\n";
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
    passthrough_args.push_back(arg);
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
  std::cout << "db: " << db_path << "\n";
  std::cout << "signals: " << db.signals.size() << "\n";
  return 0;
}

int RunTrace(int argc, char *argv[]) {
  std::optional<std::string> db_path;
  std::optional<std::string> mode;
  std::optional<std::string> signal;

  for (int i = 0; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cout
          << "Usage: rtl_trace trace --db <file> --mode <drivers|loads> --signal <hier.path>\n";
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
      mode = argv[++i];
      continue;
    }
    if (arg == "--signal") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --signal\n";
        return 1;
      }
      signal = argv[++i];
      continue;
    }
    std::cerr << "Unknown option: " << arg << "\n";
    return 1;
  }

  if (!db_path.has_value() || !mode.has_value() || !signal.has_value()) {
    std::cerr << "Missing required args: --db --mode --signal\n";
    return 1;
  }
  if (*mode != "drivers" && *mode != "loads") {
    std::cerr << "Invalid --mode: " << *mode << " (expected drivers|loads)\n";
    return 1;
  }

  TraceDb db;
  if (!LoadDb(*db_path, db)) {
    std::cerr << "Failed to read DB: " << *db_path << "\n";
    return 1;
  }

  const auto it = db.signals.find(*signal);
  if (it == db.signals.end()) {
    std::cerr << "Signal not found: " << *signal << "\n";
    return 2;
  }

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

  auto endpoint_key = [](const EndpointRecord &e) {
    return std::to_string(static_cast<int>(e.kind)) + "\t" + e.path + "\t" + e.file + "\t" +
           std::to_string(e.line) + "\t" + std::to_string(e.col) + "\t" + e.direction + "\t" +
           e.assignment_text;
  };

  const bool is_drivers_mode = (*mode == "drivers");
  std::vector<EndpointRecord> logic_endpoints;
  std::vector<EndpointRecord> unresolved_ports;
  std::unordered_set<std::string> seen_logic;
  std::unordered_set<std::string> seen_ports;
  std::unordered_set<std::string> visited_signals;

  std::function<void(const std::string &)> walk_signal = [&](const std::string &sig) {
    if (!visited_signals.insert(sig).second) return;
    const auto rec_it = db.signals.find(sig);
    if (rec_it == db.signals.end()) return;

    const std::vector<EndpointRecord> &edges =
        is_drivers_mode ? rec_it->second.drivers : rec_it->second.loads;

    for (const EndpointRecord &e : edges) {
      if (e.kind != EndpointKind::kPort) {
        const std::string key = endpoint_key(e);
        if (seen_logic.insert(key).second) logic_endpoints.push_back(e);
        continue;
      }

      bool expanded = false;
      if (e.path != sig) {
        const auto next_it = db.signals.find(e.path);
        if (next_it != db.signals.end()) {
          walk_signal(e.path);
          expanded = true;
        }
      }

      const auto &bridge_refs = is_drivers_mode ? load_refs : driver_refs;
      const auto bridge_it = bridge_refs.find(e.path);
      if (bridge_it != bridge_refs.end()) {
        for (const std::string &next_sig : bridge_it->second) {
          if (next_sig == sig) continue;
          walk_signal(next_sig);
          expanded = true;
        }
      }

      if (!expanded) {
        const std::string key = endpoint_key(e);
        if (seen_ports.insert(key).second) unresolved_ports.push_back(e);
      }
    }
  };

  walk_signal(*signal);
  std::vector<EndpointRecord> shown_results =
      logic_endpoints.empty() ? unresolved_ports : logic_endpoints;
  std::sort(shown_results.begin(), shown_results.end(),
            [](const EndpointRecord &a, const EndpointRecord &b) {
              if (a.path != b.path) return a.path < b.path;
              if (a.file != b.file) return a.file < b.file;
              if (a.line != b.line) return a.line < b.line;
              return a.col < b.col;
            });

  std::cout << "target: " << *signal << "\n";
  std::cout << "mode: " << *mode << "\n";
  std::cout << "count: " << shown_results.size() << "\n";
  for (const EndpointRecord &e : shown_results) {
    if (e.kind == EndpointKind::kPort) {
      std::cout << "port  " << e.path << " (" << e.direction << ") @ " << e.file << ":" << e.line
                << ":" << e.col << "\n";
    } else {
      std::cout << "expr  " << e.path << " @ " << e.file << ":" << e.line << ":" << e.col << "\n";
      if (*mode == "drivers" && !e.assignment_text.empty()) {
        std::cout << "  assign " << e.assignment_text << "\n";
        for (const std::string &rhs : e.rhs_signals) {
          std::cout << "  rhs    " << rhs << "\n";
        }
      }
      if (*mode == "loads" && !e.lhs_signals.empty()) {
        for (const std::string &lhs : e.lhs_signals) {
          std::cout << "  lhs    " << lhs << "\n";
        }
      }
    }
  }
  return 0;
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

  std::cerr << "Unknown subcommand: " << subcmd << "\n";
  PrintGeneralHelp();
  return 1;
}
