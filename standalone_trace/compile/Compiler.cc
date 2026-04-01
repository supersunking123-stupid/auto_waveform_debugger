#include "compile/Compiler.h"
#include "db/GraphDb.h"
#include "db/GraphDbTypes.h"
#include "db/GraphDbInternals.h"
#include "AssignmentUtils.h"

// Slang headers needed for compile-time AST walking
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
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

// Forward declarations for compile-time functions in db/GraphDb.cc
namespace slang::ast { class RootSymbol; }
namespace rtl_trace {

// Compile-time helpers defined in db/GraphDb.cc
using SymbolRefList = std::vector<const slang::ast::Symbol *>;
struct ExprTraceResult;
using TraceResult = std::variant<const slang::ast::PortSymbol *, ExprTraceResult>;
struct TraceCompileCache;

void CollectTraceableSymbols(const slang::ast::RootSymbol &root,
                             slang::flat_hash_map<std::string, const slang::ast::Symbol *> &out);
void CollectInstanceHierarchy(const slang::ast::RootSymbol &root,
                              const slang::SourceManager &sm,
                              TraceDb &db);
void BuildHierarchyFromSignals(TraceDb &db);
slang::flat_hash_map<std::string_view, size_t> BuildSubtreeSignalCounts(
    const std::vector<std::string> &keys);
std::vector<PartitionRecord> PlanHierarchyPartitions(
    const TraceDb &hier_db, const slang::flat_hash_map<std::string_view, size_t> &subtree_counts,
    size_t budget, CompileLogger *logger);
std::vector<std::vector<size_t>> BucketSignalsByPartitions(
    const std::vector<std::string> &keys,
    const std::vector<PartitionRecord> &parts);
bool SaveGraphDb(const std::string &db_path,
                 const std::vector<std::string> &keys,
                 const slang::flat_hash_map<std::string, const slang::ast::Symbol *> &symbols,
                 const slang::SourceManager &sm,
                 const TraceDb &hier_db,
                 size_t &signal_count,
                 bool low_mem,
                 CompileLogger *logger);
bool HasUnknownSysNameWarningControl(const std::vector<std::string> &args);
bool IsDollarTokenDiagnostic(const slang::Diagnostic &diag, const slang::SourceManager &sm);
bool IsDefparamRelaxDiag(slang::DiagCode code);
bool IsDefparamContextDiagnostic(const slang::Diagnostic &diag, const slang::SourceManager &sm);
bool IsIgnoredCompileDiag(const slang::Diagnostic &diag, const slang::SourceManager &sm,
                          bool relax_defparam);
bool HasBlockingCompileDiagnostics(slang::ast::Compilation &compilation, bool relax_defparam);

} // namespace rtl_trace

namespace rtl_trace {

// --- Local type for MFCU unit listing ---

struct MfcuUnitListing {
  std::vector<std::string> files;
  std::vector<std::string> includes;
  std::vector<std::string> defines;
};

// --- MFCU helpers ---

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

// --- RunCompile: main compile entry point ---

int RunCompile(int argc, char *argv[]) {
  std::string db_path = "rtl_trace.db";
  bool incremental = false;
  bool relax_defparam = false;
  bool mfcu = false;
  bool low_mem = false;
  size_t partition_budget = 0;
  std::string compile_log_path;
  std::vector<std::string> passthrough_args;

  for (int i = 0; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "Usage: rtl_trace compile [--db <file>] [--incremental] [--relax-defparam] [--mfcu] "
                   "[--low-mem] [--partition-budget <N>] [--compile-log <file>] "
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
    if (arg == "--low-mem") {
      low_mem = true;
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
  LogMem("MemBeforeElab");
  std::unique_ptr<slang::ast::Compilation> compilation = driver.createCompilation();
  driver.reportCompilation(*compilation, /*quiet*/ true);
  LogMem("MemAfterElab");
  if (HasBlockingCompileDiagnostics(*compilation, relax_defparam)) {
    if (!driver.reportDiagnostics(/*quiet*/ true)) return 1;
  }

  const slang::ast::RootSymbol &root = compilation->getRoot();
  const slang::SourceManager &sm = *compilation->getSourceManager();

  slang::flat_hash_map<std::string, const slang::ast::Symbol *> symbols;
  symbols.reserve(2000000); // Pre-allocate to prevent massive rehash spikes
  logger.Log("step: collect traceable symbols");
  LogMem("MemBeforeCollectSymbols");
  CollectTraceableSymbols(root, symbols);
  LogMem("MemAfterCollectSymbols");

  std::vector<std::string> keys;
  keys.reserve(symbols.size());
  for (const auto &[k, _] : symbols)
    keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  logger.Log("collected symbols: " + std::to_string(keys.size()));

  TraceDb hier_db;
  logger.Log("step: collect instance hierarchy");
  LogMem("MemBeforeCollectHierarchy");
  CollectInstanceHierarchy(root, sm, hier_db);
  LogMem("MemAfterCollectHierarchy");

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
  LogMem("MemBeforeSaveGraphDb");
  if (!SaveGraphDb(db_path, keys, symbols, sm, hier_db, written_signal_count, low_mem, &logger)) {
    std::cerr << "Failed to write DB: " << db_path << "\n";
    return 1;
  }
  LogMem("MemAfterSaveGraphDb");
  std::ofstream meta_out(meta_path);
  if (meta_out.is_open()) meta_out << new_fingerprint;
  logger.Log("compile done: db=" + db_path + " signals=" + std::to_string(written_signal_count));
  std::cout << "db: " << db_path << "\n";
  std::cout << "signals: " << written_signal_count << "\n";
  return 0;
}

} // namespace rtl_trace
