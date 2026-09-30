// GraphDb.cc — Shared infrastructure for standalone_trace.
// Contains: compile-time trace building, graph DB I/O, session management,
// string/path/bit utilities, CLI helpers, help printers, and query output functions.

#include "db/EntryPoints.h"
#include "db/GraphDbTypes.h"
#include "db/GraphDbInternals.h"
#include "db/ParallelTopK.h"
#include "db/CanonicalPath.h"
#include "compile/CompileData.h"
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
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/util/FlatMap.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/driver/Driver.h"
#include "slang/text/SourceManager.h"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <list>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/resource.h>
#include <malloc.h>
#include <map>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#ifdef SLANG_USE_MIMALLOC
#include <mimalloc.h>  // mi_process_info / mi_collect for RTL_TRACE_MI_STATS
#endif
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

// True when env var `name` is set to a non-empty value other than "0".
static bool EnvFlagEnabled(const char *name) {
  const char *v = std::getenv(name);
  return v != nullptr && *v != '\0' && !(v[0] == '0' && v[1] == '\0');
}

// Current RSS in KiB (finer than GetCurrentRSSMB; used by progress instrumentation).
static long GetCurrentRSSKB() {
  std::ifstream in("/proc/self/statm");
  long size = 0, resident = 0;
  if (in.is_open() && (in >> size >> resident)) {
    return resident * (sysconf(_SC_PAGE_SIZE) / 1024);
  }
  return 0;
}

// LogMem plus, when RTL_TRACE_MI_STATS=1 and mimalloc is linked in, mimalloc
// process info before and after mi_collect(true). The extra lines use a
// distinct "[Memory] mi ..." prefix; the plain LogMem line is unchanged.
static void LogMemPhase(const std::string &step) {
  LogMem(step);
#ifdef SLANG_USE_MIMALLOC
  static const bool mi_stats = EnvFlagEnabled("RTL_TRACE_MI_STATS");
  if (mi_stats) {
    auto read = [](size_t &rss, size_t &peak_rss, size_t &commit, size_t &peak_commit) {
      size_t elapsed = 0, user = 0, sys = 0, faults = 0;
      mi_process_info(&elapsed, &user, &sys, &rss, &peak_rss, &commit, &peak_commit, &faults);
    };
    size_t rss = 0, peak_rss = 0, commit = 0, peak_commit = 0;
    read(rss, peak_rss, commit, peak_commit);
    const long os_rss_before_kb = GetCurrentRSSKB();
    mi_collect(true);
    size_t rss2 = 0, peak_rss2 = 0, commit2 = 0, peak_commit2 = 0;
    read(rss2, peak_rss2, commit2, peak_commit2);
    const long os_rss_after_kb = GetCurrentRSSKB();
    constexpr size_t kMiB = 1024 * 1024;
    std::cout << "[Memory] mi " << step << " mi_rss=" << rss / kMiB << "MB mi_commit=" << commit / kMiB
              << "MB mi_peak_commit=" << peak_commit / kMiB << "MB os_rss=" << os_rss_before_kb / 1024
              << "MB | after mi_collect(true): mi_rss=" << rss2 / kMiB << "MB mi_commit=" << commit2 / kMiB
              << "MB os_rss=" << os_rss_after_kb / 1024 << "MB\n";
  }
#else
  (void)step;
#endif
}

// --- Detailed process-memory attribution (RTL_TRACE_MEM_PROGRESS=1) ---

static double MB(double bytes) { return bytes / (1024.0 * 1024.0); }
static std::string Mb1(double bytes) {
  std::ostringstream os;
  os << std::fixed << std::setprecision(0) << MB(bytes);
  return os.str();
}

// glibc malloc state + /proc/self/smaps census (which mappings hold the RSS, and whether
// mimalloc owns them). One "[Memory] proc <tag> ..." line.
static void LogProcMemDetail(const std::string &tag) {
  std::ostringstream os;
  os << "[Memory] proc " << tag;
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
  const struct mallinfo2 mi = mallinfo2();
  os << " glibc(arena=" << Mb1(static_cast<double>(mi.arena)) << "MB hblkhd=" << Mb1(static_cast<double>(mi.hblkhd))
     << "MB in_use=" << Mb1(static_cast<double>(mi.uordblks)) << "MB free=" << Mb1(static_cast<double>(mi.fordblks))
     << "MB)";
#endif
  // smaps_rollup
  {
    std::ifstream in("/proc/self/smaps_rollup");
    std::string line;
    while (std::getline(in, line)) {
      for (const char *key : {"Rss:", "Anonymous:", "Private_Dirty:", "Private_Clean:", "LazyFree:", "Shared_Clean:"}) {
        if (line.rfind(key, 0) == 0) {
          const long kb = std::strtol(line.c_str() + std::strlen(key), nullptr, 10);
          os << " " << std::string(key, std::strlen(key) - 1) << "=" << kb / 1024 << "MB";
        }
      }
    }
  }
  // smaps census
  {
    std::ifstream in("/proc/self/smaps");
    std::string line;
    struct Map { unsigned long start = 0, end = 0; long rss_kb = 0, anon_kb = 0; bool file = false; bool heap = false; };
    std::vector<Map> maps;
    Map cur;
    bool have = false;
    while (std::getline(in, line)) {
      unsigned long a = 0, b = 0;
      char perms[8] = {0};
      int n = 0;
      if (std::sscanf(line.c_str(), "%lx-%lx %7s %*s %*s %*s%n", &a, &b, perms, &n) >= 3 && line.find(':') > 20) {
        if (have) maps.push_back(cur);
        cur = Map{};
        cur.start = a;
        cur.end = b;
        have = true;
        const size_t slash = line.find_first_of("/[");
        if (slash != std::string::npos) {
          cur.heap = line.compare(slash, 6, "[heap]") == 0;
          cur.file = !cur.heap && line[slash] == '/';
        }
      } else if (have && line.rfind("Rss:", 0) == 0) {
        cur.rss_kb = std::strtol(line.c_str() + 4, nullptr, 10);
      } else if (have && line.rfind("Anonymous:", 0) == 0) {
        cur.anon_kb = std::strtol(line.c_str() + 10, nullptr, 10);
      }
    }
    if (have) maps.push_back(cur);
    double heap = 0, file = 0, anon_mi = 0, anon_other = 0;
    size_t n_mi = 0, n_other = 0;
    std::vector<const Map *> big_other;
    for (const Map &m : maps) {
      const double bytes = static_cast<double>(m.rss_kb) * 1024.0;
      if (m.heap) { heap += bytes; continue; }
      if (m.file) { file += bytes; continue; }
      bool in_mi = false;
#ifdef SLANG_USE_MIMALLOC
      in_mi = mi_is_in_heap_region(reinterpret_cast<const void *>(m.start)) ||
              mi_is_in_heap_region(reinterpret_cast<const void *>(m.start + (m.end - m.start) / 2));
#endif
      if (in_mi) { anon_mi += bytes; ++n_mi; }
      else { anon_other += bytes; ++n_other; if (m.rss_kb > 256 * 1024) big_other.push_back(&m); }
    }
    os << " maps(heap=" << Mb1(heap) << "MB file=" << Mb1(file) << "MB anon_mimalloc=" << Mb1(anon_mi) << "MB/" << n_mi
       << " anon_other=" << Mb1(anon_other) << "MB/" << n_other << ")";
    for (const Map *m : big_other) {
      os << " [other_anon " << std::hex << m->start << std::dec << " size=" << (m->end - m->start) / (1024 * 1024)
         << "MB rss=" << m->rss_kb / 1024 << "MB]";
    }
  }
#ifdef SLANG_USE_MIMALLOC
  {
    size_t elapsed = 0, user = 0, sys = 0, rss = 0, peak_rss = 0, commit = 0, peak_commit = 0, faults = 0;
    mi_process_info(&elapsed, &user, &sys, &rss, &peak_rss, &commit, &peak_commit, &faults);
    os << " mi_commit=" << Mb1(static_cast<double>(commit)) << "MB mi_peak_commit=" << Mb1(static_cast<double>(peak_commit))
       << "MB";
  }
#endif
  std::cout << os.str() << "\n";
  std::cout.flush();
}

#ifdef SLANG_USE_MIMALLOC
static void MiStatsOut(const char *msg, void *) { std::cout << "[MiStats] " << msg; }
#endif
#ifdef SLANG_USE_MIMALLOC
struct MiCensus {
  struct Cls { double live = 0, committed = 0, pages = 0; };
  std::map<size_t, Cls> by_block_size;
};
static bool MiCensusVisit(const mi_heap_t *, const mi_heap_area_t *area, void *, size_t, void *arg) {
  auto *c = static_cast<MiCensus *>(arg);
  MiCensus::Cls &k = c->by_block_size[area->block_size];
  k.live += static_cast<double>(area->used) * static_cast<double>(area->block_size);
  k.committed += static_cast<double>(area->committed);
  k.pages += 1;
  return true;
}
#endif
// Area-level (page-level) census of the mimalloc heap: live bytes vs committed bytes per block size
// class. Covers the calling thread's heap and, if MIMALLOC_VISIT_ABANDONED=1 was set at startup,
// abandoned pages of other/dead threads.
static void LogMiCensus(const std::string &tag) {
#ifdef SLANG_USE_MIMALLOC
  auto report = [&](const char *which, const MiCensus &c) {
    double live = 0, committed = 0, pages = 0;
    std::vector<std::pair<double, size_t>> top;
    for (const auto &kv : c.by_block_size) {
      live += kv.second.live;
      committed += kv.second.committed;
      pages += kv.second.pages;
      top.push_back({kv.second.live, kv.first});
    }
    std::sort(top.begin(), top.end(), std::greater<>());
    std::ostringstream os;
    os << "[Memory] micensus " << tag << " " << which << " live=" << Mb1(live) << "MB page_committed=" << Mb1(committed)
       << "MB pages=" << static_cast<long>(pages) << " top(block_size:live/committed MB):";
    for (size_t i = 0; i < top.size() && i < 10; ++i) {
      const auto &k = c.by_block_size.at(top[i].second);
      os << " " << top[i].second << ":" << Mb1(k.live) << "/" << Mb1(k.committed);
    }
    std::cout << os.str() << "\n";
  };
  MiCensus main_heap;
  mi_heap_visit_blocks(mi_heap_get_default(), false, MiCensusVisit, &main_heap);
  report("main_heap", main_heap);
  if (mi_option_is_enabled(mi_option_visit_abandoned)) {
    MiCensus ab;
    mi_abandoned_visit_blocks(mi_subproc_main(), -1, false, MiCensusVisit, &ab);
    report("abandoned", ab);
  }
  std::cout.flush();
#else
  (void)tag;
#endif
}

static void LogMiStats(const std::string &tag) {
#ifdef SLANG_USE_MIMALLOC
  static const char *mode = std::getenv("RTL_TRACE_MI_STATS");
  LogMiCensus(tag);
  if (mode != nullptr && std::string(mode) == "2") {
    std::cout << "[MiStats] ===== " << tag << " =====\n";
    mi_stats_print_out(MiStatsOut, nullptr);
  }
  std::cout.flush();
#else
  (void)tag;
#endif
}

// Heap bytes (beyond the std::string object itself) owned by a string.
static size_t StrHeap(const std::string &s) {
  return s.capacity() > 15 ? s.capacity() + 1 : 0;
}
template <typename Map>
static double FlatMapTableBytes(const Map &m) {
  // boost::unordered_flat_map: bucket_count slots of value_type plus 16 metadata bytes per 15 slots.
  return static_cast<double>(m.bucket_count()) * (sizeof(typename Map::value_type) + 16.0 / 15.0);
}

// Fine-grained build-loop profile (only active with RTL_TRACE_SAVE_GRAPH_PROFILE).
struct BuildLoopProfile {
  bool on = false;
  double index_build_s = 0;   // GetOrBuildBodyTraceIndex when the index is not yet cached (AST binding + visit)
  double port_follow_s = 0;   // CollectPortConnectionResults (parent port connection expressions)
  double resolve_s = 0;       // ResolveTraceResult
  double path_less_s = 0;     // SortUniqueSymbolsByPath (formerly per-comparison SymbolPathLess)
  size_t bodies_built = 0;
  size_t bodies_rebuilt = 0;  // builds of a body whose index was built before and later evicted
  std::unordered_set<const void *> bodies_seen;
  size_t path_less_calls = 0;
};
static BuildLoopProfile g_build_prof;
struct BuildProfTimer {
  double *acc;
  std::chrono::steady_clock::time_point t0;
  explicit BuildProfTimer(double BuildLoopProfile::*field) : acc(g_build_prof.on ? &(g_build_prof.*field) : nullptr) {
    if (acc != nullptr) t0 = std::chrono::steady_clock::now();
  }
  ~BuildProfTimer() {
    if (acc != nullptr) *acc += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  }
};

// --- Compile-time-only types (not in GraphDbTypes.h because they depend on slang AST) ---

using SymbolRefList = std::vector<const slang::ast::Symbol *>;

struct ExprTraceResult {
  const slang::ast::Expression *expr = nullptr;
  const slang::ast::Symbol *symbol = nullptr;
  const slang::ast::AssignmentExpression *assignment = nullptr;
  const slang::ast::InstanceBodySymbol *trace_body = nullptr;
  std::vector<const slang::ast::Expression *> selectors;
  SymbolRefList context_lhs_signals;
  bool context_from_instance_port = false;
  const slang::ast::InstanceSymbol *context_instance = nullptr;
  const slang::ast::PortSymbol *context_port = nullptr;
  // Struct member access metadata (Level 1). Non-empty when this result was
  // produced by resolving a MemberAccessExpression chain to a parent struct.
  std::string member_path;         // e.g. ".aw.valid"
  uint64_t member_bit_offset = 0;  // bit offset within parent struct
  uint64_t member_bit_width = 0;   // bit width of the accessed member
};

using TraceResult = std::variant<const slang::ast::PortSymbol *, ExprTraceResult>;

struct BodyTraceIndex {
  slang::flat_hash_map<const slang::ast::Symbol *, std::vector<TraceResult>> drivers;
  slang::flat_hash_map<const slang::ast::Symbol *, std::vector<TraceResult>> loads;
};

struct PerBodyTraceCache {
  slang::flat_hash_map<const slang::ast::AssignmentExpression *, SymbolRefList>
      assignment_lhs_signals;
  slang::flat_hash_map<const slang::ast::AssignmentExpression *, SymbolRefList>
      assignment_rhs_signals;
  slang::flat_hash_map<const slang::ast::Statement *, SymbolRefList> statement_lhs_signals;
  BodyTraceIndex body_trace_index;
  bool body_trace_index_ready = false;
  std::list<const slang::ast::InstanceBodySymbol *>::iterator lru_it;  // position in TraceCompileCache::lru_order
};

struct TraceCompileCache {
  slang::flat_hash_map<const slang::ast::InstanceBodySymbol *, std::unique_ptr<PerBodyTraceCache>>
      body_caches;
  // Least recently used at the front; every PerBodyTraceCache remembers its own node so a touch is O(1)
  // (the previous vector + std::find made each touch O(limit), which rules out a large limit).
  std::list<const slang::ast::InstanceBodySymbol *> lru_order;
  size_t body_cache_limit = 16;
};

#include "db/CanonicalBodiesStats.inc"  // instance-body binding statistics (RTL_TRACE_CANONICAL_STATS=1)

// ScopedFileLock — local utility for SaveGraphDb file locking
class ScopedFileLock {
 public:
  ScopedFileLock() = default;
  ScopedFileLock(const ScopedFileLock &) = delete;
  ScopedFileLock &operator=(const ScopedFileLock &) = delete;

  ~ScopedFileLock() { Release(); }

  bool Acquire(const std::string &path) {
    return AcquireImpl(path, O_RDWR | O_CREAT, LOCK_EX);
  }

  bool AcquireShared(const std::string &path) {
    return AcquireImpl(path, O_RDONLY, LOCK_SH);
  }

 private:
  bool AcquireImpl(const std::string &path, int open_flags, int lock_operation) {
    Release();
    fd_ = ::open(path.c_str(), open_flags, 0666);
    if (fd_ < 0) return false;
    if (::flock(fd_, lock_operation) != 0) {
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

  int fd_ = -1;
};

// --- Compile-time forward declarations (defined later in this file) ---

SymbolRefList CollectLhsSignalsFromStatement(const slang::ast::Statement &stmt);
const SymbolRefList &GetCachedLhsSignals(
    const slang::ast::AssignmentExpression *assignment, PerBodyTraceCache &cache);
const SymbolRefList &GetCachedRhsSignals(
    const slang::ast::AssignmentExpression *assignment, PerBodyTraceCache &cache);
const SymbolRefList &GetCachedStatementLhsSignals(
    const slang::ast::Statement &stmt, PerBodyTraceCache &cache);
EndpointRecord ResolveTraceResult(const TraceResult &r, const slang::SourceManager &sm,
                                  bool drivers_mode, TraceCompileCache *cache,
                                  CompileContext &compile_ctx,
                                  const slang::flat_hash_map<const slang::ast::Symbol *, uint32_t> *symbol_path_ids = nullptr);
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

// --- Struct member access resolution (Level 1) ---

struct MemberAccessInfo {
  const slang::ast::Symbol *parent_symbol = nullptr;
  std::string member_path;        // e.g. ".aw.valid"
  uint64_t bit_offset = 0;        // cumulative offset within parent struct
  uint64_t bit_width = 0;         // width of innermost field
  bool resolved = false;
};

// Walks a MemberAccessExpression chain (e.g. my_struct.aw.valid) back to the
// root traceable symbol (Net or Variable), accumulating bit offsets and member
// names along the way.  Only handles packed struct types in Level 1.
MemberAccessInfo ResolveStructMemberAccess(const slang::ast::MemberAccessExpression &expr) {
  MemberAccessInfo info;

  // Collect FieldSymbols from outer to inner.
  // For my_struct.aw.valid:
  //   outer MAE: member=valid, value=MAE(member=aw, value=NamedValue(my_struct))
  //   collected: [valid, aw]
  struct FieldEntry { const slang::ast::FieldSymbol *field; };
  std::vector<FieldEntry> fields;

  const slang::ast::Expression *current = &expr;
  while (auto *mae = current->as_if<slang::ast::MemberAccessExpression>()) {
    const auto *fs = mae->member.as_if<slang::ast::FieldSymbol>();
    if (fs == nullptr) return info;  // non-field member — not handled
    fields.push_back({fs});
    current = &mae->value();
  }

  // Base must be a NamedValueExpression with a traceable symbol.
  const auto *nve = current->as_if<slang::ast::NamedValueExpression>();
  if (nve == nullptr) return info;
  if (!IsTraceable(&nve->symbol)) return info;

  // Verify root is a packed struct type.
  const slang::ast::Type &root_type = nve->symbol.as_if<slang::ast::ValueSymbol>()->getType();
  const slang::ast::Type &canonical = root_type.getCanonicalType();
  if (canonical.kind != slang::ast::SymbolKind::PackedStructType) return info;

  // Reverse: fields are [valid, aw] (outer first), we need [aw, valid] for
  // correct bit-offset accumulation (outer-to-inner in source order).
  std::reverse(fields.begin(), fields.end());

  uint64_t offset = 0;
  std::string path;
  for (const auto &entry : fields) {
    offset += entry.field->bitOffset;
    path += ".";
    path += std::string(entry.field->name);
  }

  // Innermost field (last in reversed = first in original = outermost MAE member)
  const slang::ast::Type &field_type = fields.back().field->getType().getCanonicalType();

  info.parent_symbol = &nve->symbol;
  info.member_path = std::move(path);
  info.bit_offset = offset;
  info.bit_width = field_type.getBitWidth();
  info.resolved = true;
  return info;
}

// Sort + de-duplicate a symbol list by hierarchical path (the order every caller previously got from
// std::sort(..., SymbolPathLess) followed by std::unique). SymbolPathLess allocated two
// getHierarchicalPath() strings per comparison (204M comparisons / ~100 s on Lumion, mostly case/if
// statements repeating the same few LHS symbols hundreds of times). Here duplicates (same Symbol*) are
// removed first, then every remaining symbol's path is computed exactly once and the paths are sorted.
static void SortUniqueSymbolsByPath(SymbolRefList &syms) {
  if (syms.size() < 2) return;
  BuildProfTimer timer(&BuildLoopProfile::path_less_s);
  if (g_build_prof.on) g_build_prof.path_less_calls += syms.size();
  std::sort(syms.begin(), syms.end());
  syms.erase(std::unique(syms.begin(), syms.end()), syms.end());
  if (syms.size() < 2) return;
  std::vector<std::pair<std::string, const slang::ast::Symbol *>> keyed;
  keyed.reserve(syms.size());
  for (const slang::ast::Symbol *sym : syms) keyed.emplace_back(std::string(sym->getHierarchicalPath()), sym);
  std::sort(keyed.begin(), keyed.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
  for (size_t i = 0; i < keyed.size(); ++i) syms[i] = keyed[i].second;
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
  BodyTraceIndexBuilder(BodyTraceIndex &index, PerBodyTraceCache &body_cache,
                        const slang::ast::InstanceBodySymbol &body)
      : index_(index), body_cache_(body_cache), body_(body) {}

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
    SymbolRefList context_lhs = GetCachedStatementLhsSignals(stmt.ifTrue, body_cache_);
    if (stmt.ifFalse != nullptr) {
      const auto &else_lhs = GetCachedStatementLhsSignals(*stmt.ifFalse, body_cache_);
      context_lhs.insert(context_lhs.end(), else_lhs.begin(), else_lhs.end());
      SortUniqueSymbolsByPath(context_lhs);
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
      const auto &item_lhs = GetCachedStatementLhsSignals(*item.stmt, body_cache_);
      context_lhs.insert(context_lhs.end(), item_lhs.begin(), item_lhs.end());
    }
    if (stmt.defaultCase != nullptr) {
      const auto &default_lhs = GetCachedStatementLhsSignals(*stmt.defaultCase, body_cache_);
      context_lhs.insert(context_lhs.end(), default_lhs.begin(), default_lhs.end());
    }
    SortUniqueSymbolsByPath(context_lhs);
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

    timed_lhs_stack_.push_back(GetCachedStatementLhsSignals(stmt.stmt, body_cache_));
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
        result.trace_body = &body_;
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
        result.trace_body = &body_;
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
    // Try to resolve struct member access (e.g. pkt.valid → parent pkt + offset)
    auto mai = ResolveStructMemberAccess(expr);
    if (mai.resolved) {
      if constexpr (DRIVERS) {
        if (checking_instance_port_expression_ || checking_lhs_) {
          ExprTraceResult result;
          result.expr = &expr;
          result.symbol = mai.parent_symbol;
          result.trace_body = &body_;
          if (checking_lhs_) result.assignment = current_assignment_;
          result.selectors = selector_stack_;
          result.member_path = std::move(mai.member_path);
          result.member_bit_offset = mai.bit_offset;
          result.member_bit_width = mai.bit_width;
          if (checking_instance_port_expression_ && active_instance_ != nullptr && active_port_ != nullptr) {
            result.context_from_instance_port = true;
            result.context_instance = active_instance_;
            result.context_port = active_port_;
          }
          Entries()[mai.parent_symbol].push_back(std::move(result));
        }
      } else {
        if (!(checking_lhs_ && selector_depth_ == 0)) {
          ExprTraceResult result;
          result.expr = &expr;
          result.symbol = mai.parent_symbol;
          result.assignment = current_assignment_;
          result.trace_body = &body_;
          result.selectors = selector_stack_;
          result.member_path = std::move(mai.member_path);
          result.member_bit_offset = mai.bit_offset;
          result.member_bit_width = mai.bit_width;
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
          Entries()[mai.parent_symbol].push_back(std::move(result));
        }
      }
      return;
    }

    // Fall through: original behavior for non-struct member access
    const slang::ast::Symbol *sym = expr.getSymbolReference();
    if (!IsTraceable(sym)) return;
    if constexpr (DRIVERS) {
      if (checking_instance_port_expression_ || checking_lhs_) {
        ExprTraceResult result;
        result.expr = &expr;
        result.symbol = sym;
        result.trace_body = &body_;
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
        result.trace_body = &body_;
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
  PerBodyTraceCache &body_cache_;
  const slang::ast::InstanceBodySymbol &body_;
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

PerBodyTraceCache &GetOrCreateBodyTraceCache(const slang::ast::InstanceBodySymbol &body,
                                             TraceCompileCache &cache) {
  auto it = cache.body_caches.find(&body);
  if (it == cache.body_caches.end()) {
    auto [inserted_it, _] =
        cache.body_caches.emplace(&body, std::make_unique<PerBodyTraceCache>());
    it = inserted_it;
    cache.lru_order.push_back(&body);
    it->second->lru_it = std::prev(cache.lru_order.end());
  } else {
    cache.lru_order.splice(cache.lru_order.end(), cache.lru_order, it->second->lru_it);
  }
  return *it->second;
}

PerBodyTraceCache *FindBodyTraceCache(TraceCompileCache &cache,
                                      const slang::ast::InstanceBodySymbol *body) {
  if (body == nullptr) return nullptr;
  auto it = cache.body_caches.find(body);
  if (it == cache.body_caches.end()) return nullptr;
  return it->second.get();
}

void TrimTraceCompileCache(TraceCompileCache &cache) {
  while (cache.body_caches.size() > cache.body_cache_limit && !cache.lru_order.empty()) {
    const slang::ast::InstanceBodySymbol *victim = cache.lru_order.front();
    cache.lru_order.pop_front();
    cache.body_caches.erase(victim);
  }
}

void ClearTraceCompileCache(TraceCompileCache &cache) {
  cache.body_caches.clear();
  cache.lru_order.clear();
}

const BodyTraceIndex &GetOrBuildBodyTraceIndex(const slang::ast::InstanceBodySymbol &body,
                                               TraceCompileCache &cache) {
  PerBodyTraceCache &body_cache = GetOrCreateBodyTraceCache(body, cache);
  if (body_cache.body_trace_index_ready) return body_cache.body_trace_index;
  BuildProfTimer timer(&BuildLoopProfile::index_build_s);
  if (g_build_prof.on) {
    ++g_build_prof.bodies_built;
    if (!g_build_prof.bodies_seen.insert(&body).second) ++g_build_prof.bodies_rebuilt;
  }
  const long canon_rss0 = g_canon_stats.on ? CanonStatsRssKB() : 0;
  BodyTraceIndexBuilder</*DRIVERS*/ true> driver_builder(body_cache.body_trace_index, body_cache,
                                                         body);
  body.visit(driver_builder);
  BodyTraceIndexBuilder</*DRIVERS*/ false> load_builder(body_cache.body_trace_index, body_cache,
                                                        body);
  body.visit(load_builder);
  body_cache.body_trace_index_ready = true;
  if (g_canon_stats.on) CanonStatsOnBuild(body, CanonStatsRssKB() - canon_rss0);
  return body_cache.body_trace_index;
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
    auto mai = ResolveStructMemberAccess(expr);
    if (mai.resolved) {
      // Dedup on (parent_symbol, member_path) so distinct fields of the same
      // struct aren't collapsed (e.g. {pkt.valid, pkt.code[0]} on one port).
      if (!visited_member_keys_.insert({mai.parent_symbol, mai.member_path}).second)
        return;
      ExprTraceResult result;
      result.expr = &expr;
      result.symbol = mai.parent_symbol;
      result.assignment = nullptr;
      result.context_from_instance_port = true;
      result.context_instance = instance_;
      result.context_port = port_;
      result.member_path = std::move(mai.member_path);
      result.member_bit_offset = mai.bit_offset;
      result.member_bit_width = mai.bit_width;
      out_.push_back(std::move(result));
      return;
    }
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
  // Separate dedup for struct member accesses keyed on (parent, member_path)
  // so that distinct fields (e.g. pkt.valid vs pkt.code) aren't collapsed.
  std::set<std::pair<const slang::ast::Symbol *, std::string>> visited_member_keys_;  const slang::ast::InstanceSymbol *instance_ = nullptr;
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

  BuildProfTimer timer(&BuildLoopProfile::port_follow_s);
  const long canon_rss0 = g_canon_stats.on ? CanonStatsRssKB() : 0;
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
  if (g_canon_stats.on) CanonStatsOnEscape(*inst, CanonStatsRssKB() - canon_rss0);
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
  out.reserve(out.size() + it->second.size());
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
      if (!expr->member_path.empty()) {
        // Struct member through a port connection: keep the member-specific
        // result as-is.  Don't follow internalSymbol here because the
        // recursive trace operates at whole-struct granularity and would
        // lose member specificity.  Multiple distinct members of the same
        // port (e.g. {pkt.valid, pkt.code[0]}) each produce their own
        // endpoint because we do NOT mark the internal symbol as visited.
        // (followed stays false so the member result is pushed below.)
      } else {
        const slang::ast::Symbol *internal = expr->context_port->internalSymbol;
        if (visited.insert(internal).second) {
          CanonStatsReasonScope canon_reason(CanonicalStats::kDownward);
          std::vector<TraceResult> nested = ComputeIndexedTraceResults<DRIVERS>(internal, cache, visited);
          if (!nested.empty()) {
            out.insert(out.end(), std::make_move_iterator(nested.begin()), std::make_move_iterator(nested.end()));
            followed = true;
          }
        }
      }
    }
    if (!followed && expr != nullptr) out.push_back(*expr);
  }
  return out;
}

SignalRecord BuildSignalRecord(const slang::ast::Symbol *sym, const slang::SourceManager &sm,
                               TraceCompileCache &cache,
                               CompileContext &compile_ctx,
                               const slang::flat_hash_map<const slang::ast::Symbol *, uint32_t> *symbol_path_ids = nullptr) {
  CanonStatsSignalScope canon_stats_scope(sym);
  SignalRecord rec;
  std::unordered_set<const slang::ast::Symbol *> visited_drivers;
  visited_drivers.insert(sym);
  for (const TraceResult &r : ComputeIndexedTraceResults</*DRIVERS*/ true>(sym, cache, visited_drivers)) {
    BuildProfTimer timer(&BuildLoopProfile::resolve_s);
    rec.drivers.push_back(
        std::move(ResolveTraceResult(r, sm, true, &cache, compile_ctx, symbol_path_ids)));
  }

  std::unordered_set<const slang::ast::Symbol *> visited_loads;
  visited_loads.insert(sym);
  for (const TraceResult &r : ComputeIndexedTraceResults</*DRIVERS*/ false>(sym, cache, visited_loads)) {
    BuildProfTimer timer(&BuildLoopProfile::resolve_s);
    rec.loads.push_back(
        std::move(ResolveTraceResult(r, sm, false, &cache, compile_ctx, symbol_path_ids)));
  }

  return rec;
}

#include "db/CanonicalBodies.inc"  // canonical-body tracer (default; RTL_TRACE_CANONICAL_BODIES=0 disables)

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

slang::SourceLocation GetPhysicalSourceLoc(slang::SourceLocation loc,
                                           const slang::SourceManager &sm) {
  if (!loc.valid()) return {};
  const slang::SourceLocation original = sm.getFullyOriginalLoc(loc);
  return original.valid() ? original : loc;
}

std::string NormalizeSourcePathString(std::string_view path, CompileContext &compile_ctx) {
  if (path.empty()) return "";
  std::string key(path);
  auto it = compile_ctx.source_info_cache.normalized_path_cache.find(key);
  if (it != compile_ctx.source_info_cache.normalized_path_cache.end()) return it->second;

  std::filesystem::path normalized(path);
  std::error_code ec;
  if (!normalized.is_absolute()) {
    std::filesystem::path abs = std::filesystem::absolute(normalized, ec);
    if (!ec) normalized = std::move(abs);
    ec.clear();
  }
  if (std::filesystem::exists(normalized, ec)) {
    std::filesystem::path canon = std::filesystem::weakly_canonical(normalized, ec);
    if (!ec) normalized = std::move(canon);
  }

  auto [inserted_it, _] =
      compile_ctx.source_info_cache.normalized_path_cache.emplace(std::move(key), normalized.lexically_normal().string());
  return inserted_it->second;
}

std::string GetAbsoluteSourcePath(slang::SourceLocation loc, const slang::SourceManager &sm,
                                  CompileContext &compile_ctx) {
  const slang::SourceLocation file_loc = GetPhysicalSourceLoc(loc, sm);
  if (!file_loc.valid()) return "";

  const uint32_t buffer_id = file_loc.buffer().getId();
  auto path_it = compile_ctx.source_info_cache.physical_path_by_buffer.find(buffer_id);
  if (path_it != compile_ctx.source_info_cache.physical_path_by_buffer.end()) return path_it->second;

  std::string path;
  const std::filesystem::path &full_path = sm.getFullPath(file_loc.buffer());
  if (!full_path.empty()) {
    path = full_path.string();
  } else {
    const std::string_view raw_name = sm.getRawFileName(file_loc.buffer());
    if (!raw_name.empty()) {
      path = NormalizeSourcePathString(raw_name, compile_ctx);
    } else {
      path = NormalizeSourcePathString(sm.getFileName(file_loc), compile_ctx);
    }
  }

  compile_ctx.source_info_cache.physical_path_by_buffer.emplace(buffer_id, path);
  return path;
}

int GetPhysicalSourceLine(slang::SourceLocation loc, const slang::SourceManager &sm,
                          CompileContext &compile_ctx) {
  const slang::SourceLocation file_loc = GetPhysicalSourceLoc(loc, sm);
  if (!file_loc.valid()) return 0;

  const uint32_t buffer_id = file_loc.buffer().getId();
  const std::string_view text = sm.getSourceText(file_loc.buffer());
  if (file_loc.offset() > text.size()) return static_cast<int>(sm.getLineNumber(file_loc));

  auto [it, inserted] = compile_ctx.source_info_cache.line_start_offsets_by_buffer.try_emplace(buffer_id);
  std::vector<size_t> &line_starts = it->second;
  if (inserted) {
    line_starts.push_back(0);
    for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\n') {
        line_starts.push_back(i + 1);
      } else if (text[i] == '\r') {
        if (i + 1 < text.size() && text[i + 1] == '\n') i++;
        line_starts.push_back(i + 1);
      }
    }
  }

  const auto upper = std::upper_bound(line_starts.begin(), line_starts.end(), file_loc.offset());
  const size_t line = static_cast<size_t>(upper - line_starts.begin());
  return static_cast<int>(std::min<size_t>(std::max<size_t>(line, 1),
                                           static_cast<size_t>(std::numeric_limits<int>::max())));
}

std::string GetStoredSourcePath(slang::SourceLocation loc, const slang::SourceManager &sm,
                                CompileContext &compile_ctx) {
  if (compile_ctx.source_path_mode == SourcePathMode::kPhysicalAbsolute)
    return GetAbsoluteSourcePath(loc, sm, compile_ctx);
  if (!loc.valid()) return "";
  return std::string(sm.getFileName(loc));
}

int GetStoredSourceLine(slang::SourceLocation loc, const slang::SourceManager &sm,
                        CompileContext &compile_ctx) {
  if (compile_ctx.source_path_mode == SourcePathMode::kPhysicalAbsolute)
    return GetPhysicalSourceLine(loc, sm, compile_ctx);
  return static_cast<int>(sm.getLineNumber(loc));
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

uint32_t InternString(std::string_view sv, std::vector<std::string> &pool,
                      slang::flat_hash_map<std::string_view, uint32_t> &index) {
  auto it = index.find(sv);
  if (it != index.end()) return it->second;
  const uint32_t id = static_cast<uint32_t>(pool.size());
  // If the pool would reallocate, all existing string_view keys in the index
  // would dangle.  Re-emit them into the new buffer before inserting.
  if (pool.size() == pool.capacity()) {
    pool.emplace_back(sv);
    index.clear();
    for (size_t i = 0; i < pool.size(); ++i)
      index.emplace(std::string_view(pool[i]), static_cast<uint32_t>(i));
    return id;
  }
  pool.emplace_back(sv);
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

// Parses a bit_map that is exactly one "[N]" or "[L:R]" select. Anything else
// (multi-dimensional "[i][j]", symbolic "[i-1]", ...) is rejected, so the
// merge pass never rewrites such text.
std::optional<std::pair<int32_t, int32_t>> ParseExactBitMapText(std::string_view bit_map) {
  if (bit_map.size() < 3 || bit_map.front() != '[' || bit_map.back() != ']') return std::nullopt;
  const std::string_view inside = bit_map.substr(1, bit_map.size() - 2);
  const auto parse_int = [](std::string_view text) -> std::optional<int32_t> {
    int32_t v = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (ec != std::errc() || ptr != text.data() + text.size()) return std::nullopt;
    return v;
  };
  const size_t colon = inside.find(':');
  if (colon == std::string_view::npos) {
    const auto v = parse_int(inside);
    if (!v.has_value()) return std::nullopt;
    return std::make_pair(*v, *v);
  }
  const auto l = parse_int(inside.substr(0, colon));
  const auto r = parse_int(inside.substr(colon + 1));
  if (!l.has_value() || !r.has_value()) return std::nullopt;
  return std::make_pair(*l, *r);
}

std::string FormatBitRange(int32_t hi, int32_t lo) {
  if (hi == lo) return "[" + std::to_string(hi) + "]";
  return "[" + std::to_string(hi) + ":" + std::to_string(lo) + "]";
}

// ---------------------------------------------------------------------------
// Endpoint bit-range merging.
//
// Endpoints of one signal that come from the same assignment (same kind, path,
// file, line, direction, assignment range/text and lhs/rhs signal lists) and
// whose exact bit ranges are adjacent or overlapping collapse into one endpoint
// with the union range: per-bit assignments and generate loops such as
// `assign y[i] = ...` for i = 0..7 become a single `[7:0]` endpoint, and exact
// duplicates of an endpoint are dropped.
//
// The pass works in place and never moves an endpoint while grouping keys
// point into it. Output order is the input (source) order: each merged endpoint
// takes the position of its first member, and endpoints that do not merge keep
// their bit_map text unchanged. Hash-map iteration order is never observed.
struct EndpointMergeKey {
  const EndpointRecord *e;

  bool operator==(const EndpointMergeKey &o) const {
    const EndpointRecord &a = *e;
    const EndpointRecord &b = *o.e;
    return a.kind == b.kind && a.path_id == b.path_id && a.file_id == b.file_id &&
           a.line == b.line && a.has_assignment_range == b.has_assignment_range &&
           a.assignment_start == b.assignment_start && a.assignment_end == b.assignment_end &&
           a.path == b.path && a.file == b.file && a.direction == b.direction &&
           a.assignment_text == b.assignment_text && a.lhs_signal_ids == b.lhs_signal_ids &&
           a.rhs_signal_ids == b.rhs_signal_ids && a.lhs_signals == b.lhs_signals &&
           a.rhs_signals == b.rhs_signals;
  }
};

struct EndpointMergeKeyHash {
  size_t operator()(const EndpointMergeKey &k) const {
    const EndpointRecord &e = *k.e;
    auto mix = [](size_t h, size_t v) { return h ^ (v + 0x9e3779b9 + (h << 6) + (h >> 2)); };
    size_t h = std::hash<std::string_view>()(e.path);
    h = mix(h, e.path_id);
    h = mix(h, static_cast<size_t>(e.line));
    h = mix(h, e.assignment_start);
    h = mix(h, std::hash<std::string_view>()(e.assignment_text));
    return h;
  }
};

struct EndpointMergeScratch {
  struct Item {
    uint32_t group = 0;
    int32_t lo = 0;
    int32_t hi = 0;
    uint32_t index = 0;
    bool ascending = false;  // written as [lo:hi] with lo < hi
  };
  slang::flat_hash_map<EndpointMergeKey, uint32_t, EndpointMergeKeyHash> group_of;
  std::vector<Item> items;
  std::vector<uint8_t> dropped;
};

void MergeEndpointBitRangesInPlace(std::vector<EndpointRecord> &endpoints, EndpointMergeScratch &scratch) {
  if (endpoints.size() < 2) return;
  auto &items = scratch.items;
  items.clear();
  for (size_t i = 0; i < endpoints.size(); ++i) {
    const EndpointRecord &e = endpoints[i];
    if (e.bit_map.empty() || e.bit_map_approximate) continue;
    const auto parsed = ParseExactBitMapText(e.bit_map);
    if (!parsed.has_value()) continue;
    EndpointMergeScratch::Item item;
    item.lo = std::min(parsed->first, parsed->second);
    item.hi = std::max(parsed->first, parsed->second);
    item.index = static_cast<uint32_t>(i);
    item.ascending = parsed->first < parsed->second;
    items.push_back(item);
  }
  if (items.size() < 2) return;

  // Group ids are assigned in order of first appearance, so the result does not
  // depend on hash-map iteration order.
  auto &group_of = scratch.group_of;
  group_of.clear();
  for (EndpointMergeScratch::Item &item : items) {
    const auto [it, inserted] =
        group_of.try_emplace(EndpointMergeKey{&endpoints[item.index]}, static_cast<uint32_t>(group_of.size()));
    item.group = it->second;
  }
  if (group_of.size() == items.size()) return;  // every mergeable endpoint is alone in its group

  std::sort(items.begin(), items.end(), [](const auto &a, const auto &b) {
    if (a.group != b.group) return a.group < b.group;
    if (a.lo != b.lo) return a.lo < b.lo;
    if (a.hi != b.hi) return a.hi < b.hi;
    return a.index < b.index;
  });
  auto &dropped = scratch.dropped;
  dropped.assign(endpoints.size(), 0);
  bool any_dropped = false;
  size_t i = 0;
  while (i < items.size()) {
    const size_t run_begin = i;
    int32_t cur_lo = items[i].lo;
    int32_t cur_hi = items[i].hi;
    uint32_t first_index = items[i].index;
    bool any_ascending = items[i].ascending;
    bool any_descending = items[i].lo != items[i].hi && !items[i].ascending;
    ++i;
    while (i < items.size() && items[i].group == items[run_begin].group &&
           static_cast<int64_t>(items[i].lo) <= static_cast<int64_t>(cur_hi) + 1) {
      cur_hi = std::max(cur_hi, items[i].hi);
      first_index = std::min(first_index, items[i].index);
      any_ascending = any_ascending || items[i].ascending;
      any_descending = any_descending || (items[i].lo != items[i].hi && !items[i].ascending);
      ++i;
    }
    if (i - run_begin < 2) continue;  // nothing merged: keep the endpoint untouched
    for (size_t k = run_begin; k < i; ++k) {
      if (items[k].index != first_index) dropped[items[k].index] = 1;
    }
    any_dropped = true;
    // Keep the source's range direction when every multi-bit member used [lo:hi].
    endpoints[first_index].bit_map = (any_ascending && !any_descending && cur_lo != cur_hi)
                                         ? "[" + std::to_string(cur_lo) + ":" + std::to_string(cur_hi) + "]"
                                         : FormatBitRange(cur_hi, cur_lo);
  }
  if (!any_dropped) return;
  size_t out = 0;
  for (size_t k = 0; k < endpoints.size(); ++k) {
    if (dropped[k]) continue;
    if (out != k) endpoints[out] = std::move(endpoints[k]);
    ++out;
  }
  endpoints.resize(out);
}

constexpr size_t kCompactGlobalNetThreshold = 1024;

static bool CaseInsensitiveContains(std::string_view haystack, std::string_view needle) {
  if (needle.size() > haystack.size()) return false;
  for (size_t i = 0; i <= haystack.size() - needle.size(); ++i) {
    bool match = true;
    for (size_t j = 0; j < needle.size(); ++j) {
      if (std::tolower(static_cast<unsigned char>(haystack[i + j])) != needle[j]) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

static bool CaseInsensitiveEquals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != b[i]) return false;
  }
  return true;
}

bool LooksLikeClockOrResetName(std::string_view path) {
  const std::string_view leaf = LeafName(path);
  if (CaseInsensitiveEquals(leaf, "clk") || CaseInsensitiveEquals(leaf, "clock") ||
      CaseInsensitiveEquals(leaf, "rst") || CaseInsensitiveEquals(leaf, "reset") ||
      CaseInsensitiveEquals(leaf, "rst_n") || CaseInsensitiveEquals(leaf, "reset_n") ||
      CaseInsensitiveEquals(leaf, "resetn") || CaseInsensitiveEquals(leaf, "rstn"))
    return true;
  return CaseInsensitiveContains(leaf, "clk") || CaseInsensitiveContains(leaf, "clock") ||
         CaseInsensitiveContains(leaf, "rst") || CaseInsensitiveContains(leaf, "reset");
}

std::string ClassifyGlobalNetCategory(std::string_view path) {
  const std::string_view leaf = LeafName(path);
  if (CaseInsensitiveContains(leaf, "rst") || CaseInsensitiveContains(leaf, "reset"))
    return "reset";
  return "clock";
}

bool ShouldCompactGlobalNet(std::string_view path, size_t load_count) {
  return load_count >= kCompactGlobalNetThreshold && LooksLikeClockOrResetName(path);
}

// Sorted, deduplicated sink paths of a compacted global net. The views point into graph.strings and
// into `loads`, so they are valid only while `loads` is alive and unchanged.
std::vector<std::string_view> ExtractCompactSinkPaths(const std::string &source, const GraphDb &graph,
                                                      const std::vector<EndpointRecord> &loads) {
  std::vector<std::string_view> sinks;
  sinks.reserve(loads.size());
  for (const EndpointRecord &e : loads) {
    bool has_lhs_refs = false;
    if (!e.lhs_signal_ids.empty()) {
      has_lhs_refs = true;
      for (uint32_t path_id : e.lhs_signal_ids) {
        const std::string &path = GraphString(graph, path_id);
        if (!path.empty()) sinks.push_back(path);
      }
    }
    if (!e.lhs_signals.empty()) {
      has_lhs_refs = true;
      sinks.insert(sinks.end(), e.lhs_signals.begin(), e.lhs_signals.end());
    }
    if (has_lhs_refs) {
      continue;
    }
    if (!e.path.empty() && e.path != source) sinks.push_back(e.path);
  }
  std::sort(sinks.begin(), sinks.end());
  sinks.erase(std::unique(sinks.begin(), sinks.end()), sinks.end());
  return sinks;
}

bool TryParseSimpleInt(std::string_view s, int64_t &out) {
  // Skip leading whitespace
  size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
    ++start;
  // Skip trailing whitespace
  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
    --end;
  if (start >= end) return false;
  auto [ptr, ec] = std::from_chars(s.data() + start, s.data() + end, out);
  return ec == std::errc() && ptr == s.data() + end;
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
      auto mai = ResolveStructMemberAccess(expr);
      if (mai.resolved) { out_.push_back(mai.parent_symbol); return; }
      if (const slang::ast::Symbol *sym = expr.getSymbolReference(); IsTraceable(sym)) out_.push_back(sym);
    }

   private:
    SymbolRefList &out_;
  };

  SymbolRefList paths;
  RhsSignalCollector collector(paths);
  assignment->right().visit(collector);
  SortUniqueSymbolsByPath(paths);
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
      auto mai = ResolveStructMemberAccess(expr);
      if (mai.resolved) { out_.push_back(mai.parent_symbol); return; }
      if (const slang::ast::Symbol *sym = expr.getSymbolReference(); IsTraceable(sym)) out_.push_back(sym);
    }

   private:
    SymbolRefList &out_;
  };

  SymbolRefList paths;
  LhsSignalCollector collector(paths);
  assignment->left().visit(collector);
  SortUniqueSymbolsByPath(paths);
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
          auto mai = ResolveStructMemberAccess(expr);
          if (mai.resolved) { out_.push_back(mai.parent_symbol); return; }
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
  SortUniqueSymbolsByPath(paths);
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

const SymbolRefList &GetCachedLhsSignals(
    const slang::ast::AssignmentExpression *assignment, PerBodyTraceCache &cache) {
  static const SymbolRefList empty;
  if (assignment == nullptr) return empty;
  auto it = cache.assignment_lhs_signals.find(assignment);
  if (it != cache.assignment_lhs_signals.end()) return it->second;
  return cache.assignment_lhs_signals.emplace(assignment, CollectLhsSignals(assignment)).first->second;
}

const SymbolRefList &GetCachedRhsSignals(
    const slang::ast::AssignmentExpression *assignment, PerBodyTraceCache &cache) {
  static const SymbolRefList empty;
  if (assignment == nullptr) return empty;
  auto it = cache.assignment_rhs_signals.find(assignment);
  if (it != cache.assignment_rhs_signals.end()) return it->second;
  return cache.assignment_rhs_signals.emplace(assignment, CollectRhsSignals(assignment)).first->second;
}

const SymbolRefList &GetCachedStatementLhsSignals(
    const slang::ast::Statement &stmt, PerBodyTraceCache &cache) {
  auto it = cache.statement_lhs_signals.find(&stmt);
  if (it != cache.statement_lhs_signals.end()) return it->second;
  return cache.statement_lhs_signals.emplace(&stmt, CollectLhsSignalsFromStatement(stmt)).first->second;
}

void MaterializeSignalRefs(
    const SymbolRefList &signals,
    const slang::flat_hash_map<const slang::ast::Symbol *, uint32_t> *symbol_path_ids,
    std::vector<uint32_t> &id_out, std::vector<std::string> &path_out) {
  id_out.clear();
  path_out.clear();
  id_out.reserve(signals.size());
  for (const slang::ast::Symbol *sym : signals) {
    if (sym == nullptr) continue;
    if (symbol_path_ids != nullptr) {
      auto it = symbol_path_ids->find(sym);
      if (it != symbol_path_ids->end()) {
        id_out.push_back(it->second);
        continue;
      }
    }
    path_out.push_back(std::string(sym->getHierarchicalPath()));
  }
}

void InsertSortedUniqueString(std::vector<std::string> &paths, std::string value) {
  auto it = std::lower_bound(paths.begin(), paths.end(), value);
  if (it != paths.end() && *it == value) return;
  paths.insert(it, std::move(value));
}

#ifndef NDEBUG
bool AreSortedUniqueSignalIdsByPath(const std::vector<uint32_t> &signal_ids, const GraphDb &graph) {
  for (size_t i = 1; i < signal_ids.size(); ++i) {
    const std::string &prev = GraphString(graph, signal_ids[i - 1]);
    const std::string &cur = GraphString(graph, signal_ids[i]);
    if (!(prev < cur)) return false;
  }
  return true;
}

template <typename T>
bool AreSortedUniqueValues(const std::vector<T> &values) {
  return std::adjacent_find(values.begin(), values.end(),
                            [](const T &lhs, const T &rhs) { return !(lhs < rhs); }) == values.end();
}
#endif

EndpointRecord ResolveTraceResult(const TraceResult &r, const slang::SourceManager &sm, bool drivers_mode,
                                  TraceCompileCache *cache,
                                  CompileContext &compile_ctx,
                                  const slang::flat_hash_map<const slang::ast::Symbol *, uint32_t> *symbol_path_ids) {
  EndpointRecord rec;
  std::visit(
      [&](const auto &item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, const slang::ast::PortSymbol *>) {
          rec.kind = EndpointKind::kPort;
          // Port endpoints are usually not in the symbol cache, so use getHierarchicalPath().
          rec.path = item->getHierarchicalPath();
          rec.direction = DirectionToString(item->direction);
          const auto loc = item->location;
          rec.file = GetStoredSourcePath(loc, sm, compile_ctx);
          rec.line = GetStoredSourceLine(loc, sm, compile_ctx);
        } else {
          rec.kind = EndpointKind::kExpr;
          PerBodyTraceCache *body_cache =
              (cache != nullptr) ? FindBodyTraceCache(*cache, item.trace_body) : nullptr;
          if (item.symbol != nullptr && symbol_path_ids != nullptr) {
            auto it = symbol_path_ids->find(item.symbol);
            if (it != symbol_path_ids->end()) {
              rec.path_id = it->second;
            }
          }
          if (rec.path_id == std::numeric_limits<uint32_t>::max() || !item.member_path.empty()) {
            // Build path string directly: parent hierarchical path + member suffix
            // (path_id points to the parent struct name, missing the member part)
            rec.path = item.symbol != nullptr ? item.symbol->getHierarchicalPath() : "";
            rec.path += item.member_path;
            rec.path_id = std::numeric_limits<uint32_t>::max();  // use string, not ID
          }
          const auto loc = item.expr->sourceRange.start();
          rec.file = GetStoredSourcePath(loc, sm, compile_ctx);
          rec.line = GetStoredSourceLine(loc, sm, compile_ctx);
          if (item.symbol != nullptr) {
            if (item.member_bit_width > 0) {
              // Struct member access: compute absolute bit ranges by adding
              // member_bit_offset to each selector's evaluated range.
              if (item.selectors.empty()) {
                // No sub-select: full member range
                uint64_t lo = item.member_bit_offset;
                uint64_t hi = lo + item.member_bit_width - 1;
                rec.bit_map = (hi == lo)
                    ? "[" + std::to_string(hi) + "]"
                    : "[" + std::to_string(hi) + ":" + std::to_string(lo) + "]";
                rec.bit_map_approximate = false;
              } else {
                // Sub-select within member (e.g. pkt.data[1:0]):
                // selectors are relative to the member field; add member offset
                // to get absolute positions within the parent struct.
                bool approximate = false;
                for (const slang::ast::Expression *sel_expr : item.selectors) {
                  slang::ast::EvalContext eval_ctx(*item.symbol);
                  if (auto range = sel_expr->evalSelector(eval_ctx, false)) {
                    int64_t abs_left  = range->left  + static_cast<int64_t>(item.member_bit_offset);
                    int64_t abs_right = range->right + static_cast<int64_t>(item.member_bit_offset);
                    rec.bit_map += (abs_left == abs_right)
                        ? "[" + std::to_string(abs_left) + "]"
                        : "[" + std::to_string(abs_left) + ":" + std::to_string(abs_right) + "]";
                    continue;
                  }
                  // Non-constant selector — fall back to source text with member range prefix
                  approximate = true;
                  if (const auto *sel = sel_expr->as_if<slang::ast::ElementSelectExpression>()) {
                    rec.bit_map += "[" + GetSourceText(sel->selector().sourceRange, sm) + "]";
                  } else if (const auto *sel = sel_expr->as_if<slang::ast::RangeSelectExpression>()) {
                    rec.bit_map += "[" + GetSourceText(sel->left().sourceRange, sm) + ":"
                                + GetSourceText(sel->right().sourceRange, sm) + "]";
                  } else {
                    rec.bit_map += "[" + GetSourceText(sel_expr->sourceRange, sm) + "]";
                  }
                }
                rec.bit_map_approximate = approximate;
              }
            } else {
              // Normal (non-member) case
              auto bit_desc = DescribeBitSelectors(item.selectors, sm, *item.symbol);
              rec.bit_map = std::move(bit_desc.first);
              rec.bit_map_approximate = bit_desc.second;
            }
          }
          if (item.assignment != nullptr) {
            if (auto off = GetSourceOffsetRange(item.assignment->sourceRange, sm); off.has_value()) {
              rec.has_assignment_range = true;
              rec.assignment_start = off->first;
              rec.assignment_end = off->second;
            }
            if (body_cache != nullptr) {
              MaterializeSignalRefs(GetCachedLhsSignals(item.assignment, *body_cache), symbol_path_ids,
                                    rec.lhs_signal_ids, rec.lhs_signals);
              MaterializeSignalRefs(GetCachedRhsSignals(item.assignment, *body_cache), symbol_path_ids,
                                    rec.rhs_signal_ids, rec.rhs_signals);
            } else {
              MaterializeSignalRefs(CollectLhsSignals(item.assignment), symbol_path_ids,
                                    rec.lhs_signal_ids, rec.lhs_signals);
              MaterializeSignalRefs(CollectRhsSignals(item.assignment), symbol_path_ids,
                                    rec.rhs_signal_ids, rec.rhs_signals);
            }
            // The graph DB stores source offsets, not raw assignment text. When exact
            // LHS refs are already available, keep only the offsets here and let query
            // paths reconstruct the text on demand via MaterializeAssignmentTexts().
            if (!rec.has_assignment_range ||
                (rec.lhs_signal_ids.empty() && rec.lhs_signals.empty())) {
              rec.assignment_text = GetSourceText(item.assignment->sourceRange, sm);
            }
          } else if (!item.context_lhs_signals.empty()) {
            MaterializeSignalRefs(item.context_lhs_signals, symbol_path_ids,
                                  rec.lhs_signal_ids, rec.lhs_signals);
          }
          if (item.context_from_instance_port && item.context_instance != nullptr && item.context_port != nullptr) {
            const std::string port_signal = MakeInstancePortPath(item.context_instance, item.context_port);
            if (drivers_mode) {
              InsertSortedUniqueString(rec.rhs_signals, port_signal);
            } else {
              InsertSortedUniqueString(rec.lhs_signals, port_signal);
            }
          }
        }
      },
      r);
  return rec;
}

void CollectTraceableSymbols(const slang::ast::RootSymbol &root,
                             std::vector<SignalCompileItem> &out,
                             std::vector<std::string> &out_paths) {
  // Collected as (path, item) pairs so the sort/unique below behaves exactly as when the path was a
  // SignalCompileItem field (same comparisons in the same order => same surviving duplicates and
  // order), then split into the two parallel output vectors.
  struct CollectedSignal {
    std::string path;
    SignalCompileItem item;
  };
  std::vector<CollectedSignal> collected;
  collected.reserve(out.capacity());
  auto collect_from_scope = [&](const slang::ast::Scope &scope) {
    for (const auto &net : scope.membersOfType<slang::ast::NetSymbol>()) {
      SignalCompileItem item;
      item.sym = &net;
      item.body = GetContainingInstance(&net);
      collected.push_back(CollectedSignal{std::string(net.getHierarchicalPath()), item});
    }
    for (const auto &var : scope.membersOfType<slang::ast::VariableSymbol>()) {
      SignalCompileItem item;
      item.sym = &var;
      item.body = GetContainingInstance(&var);
      collected.push_back(CollectedSignal{std::string(var.getHierarchicalPath()), item});
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
  std::sort(collected.begin(), collected.end(), [](const CollectedSignal &lhs, const CollectedSignal &rhs) {
    return lhs.path < rhs.path;
  });
  collected.erase(std::unique(collected.begin(), collected.end(),
                              [](const CollectedSignal &lhs, const CollectedSignal &rhs) {
                                return lhs.path == rhs.path;
                              }),
                  collected.end());
  for (const CollectedSignal &c : collected) out.push_back(c.item);
  // Same headroom as `out` (the caller's reserve policy) for the struct members appended later.
  out_paths.reserve(out.capacity());
  for (CollectedSignal &c : collected) out_paths.push_back(std::move(c.path));
}

// --- Struct member decomposition (Level 2) ---

void DecomposePackedStructFields(std::vector<SignalCompileItem> &signals,
                                  std::vector<std::string> &paths,
                                  uint32_t parent_idx,
                                  int max_depth,
                                  int current_depth,
                                  uint64_t cumulative_offset) {
  // Copy what we need from the parent instead of holding a reference into
  // `signals`: the push_back below can reallocate the vector, which would leave
  // a `const SignalCompileItem&` dangling for the next loop iteration.
  const slang::ast::Symbol *const parent_sym = signals[parent_idx].sym;
  if (parent_sym == nullptr) return;
  const std::string parent_path = paths[parent_idx];
  const slang::ast::InstanceBodySymbol *const parent_body = signals[parent_idx].body;

  const auto *vs = parent_sym->as_if<slang::ast::ValueSymbol>();
  if (vs == nullptr) return;

  const slang::ast::Type &canonical = vs->getType().getCanonicalType();
  if (canonical.kind != slang::ast::SymbolKind::PackedStructType) return;

  const auto *pst = canonical.as_if<slang::ast::PackedStructType>();
  if (pst == nullptr) return;
  const slang::ast::Scope &scope = *pst;
  for (const auto &field : scope.membersOfType<slang::ast::FieldSymbol>()) {
    const uint64_t field_offset = cumulative_offset + field.bitOffset;
    const slang::ast::Type &field_type = field.getType().getCanonicalType();
    const uint64_t field_width = field_type.getBitWidth();

    SignalCompileItem item;
    item.sym = parent_sym;   // reuse parent's AST symbol for BuildSignalRecord
    item.body = parent_body;
    item.parent_signal_idx = parent_idx;
    item.member_bit_offset = field_offset;
    item.member_bit_width = field_width;
    item.struct_depth = current_depth;

    const uint32_t child_idx = static_cast<uint32_t>(signals.size());
    signals.push_back(item);
    paths.push_back(parent_path + "." + std::string(field.name));

    // Recurse into nested packed structs
    if (current_depth < max_depth &&
        field_type.kind == slang::ast::SymbolKind::PackedStructType) {
      DecomposePackedStructFields(signals, paths, child_idx, max_depth, current_depth + 1, field_offset);
    }
  }
}

void DecomposeStructMembers(std::vector<SignalCompileItem> &signals, std::vector<std::string> &paths,
                            int max_depth) {
  const size_t original_count = signals.size();
  for (size_t i = 0; i < original_count; ++i) {
    const SignalCompileItem &item = signals[i];
    if (item.sym == nullptr) continue;
    if (item.parent_signal_idx != std::numeric_limits<uint32_t>::max()) continue;  // already a member

    const auto *vs = item.sym->as_if<slang::ast::ValueSymbol>();
    if (vs == nullptr) continue;

    const slang::ast::Type &canonical = vs->getType().getCanonicalType();
    if (canonical.kind != slang::ast::SymbolKind::PackedStructType) continue;

    DecomposePackedStructFields(signals, paths, static_cast<uint32_t>(i), max_depth, 1, 0);
  }
}

void CollectInstanceHierarchy(const slang::ast::RootSymbol &root, const slang::SourceManager &sm,
                              TraceDb &db, CompileContext &compile_ctx) {
  // No blanket reserve here: a flat map touches (nearly) every page of a reserved table, so reserving
  // 2M nodes cost ~0.5 GB RSS on Lumion (707k instances). The caller pre-sizes it from the signal count.
  auto note_instance = [&](const slang::ast::InstanceSymbol &inst) {
    auto &node = db.hierarchy[std::string(inst.getHierarchicalPath())];
    node.module = std::string(inst.getDefinition().name);
    const auto loc = inst.getDefinition().location;
    if (loc.valid()) {
      node.source_file = GetStoredSourcePath(loc, sm, compile_ctx);
      node.source_line = static_cast<uint32_t>(std::max(GetStoredSourceLine(loc, sm, compile_ctx), 0));
    }
    const auto params = inst.body.getParameters();
    node.parameters.clear();
    node.parameters.reserve(params.size());
    for (const slang::ast::ParameterSymbolBase *param_base : params) {
      InstanceParameterRecord param;
      param.name = std::string(param_base->symbol.name);
      param.is_local = param_base->isLocalParam();
      param.is_port = param_base->isPortParam();
      if (param_base->symbol.kind == slang::ast::SymbolKind::Parameter) {
        const auto &value_param = param_base->symbol.as<slang::ast::ParameterSymbol>();
        param.kind = InstanceParameterKind::kValue;
        param.value = value_param.getValue().toString();
        param.is_overridden = value_param.isOverridden();
      } else if (param_base->symbol.kind == slang::ast::SymbolKind::TypeParameter) {
        const auto &type_param = param_base->symbol.as<slang::ast::TypeParameterSymbol>();
        param.kind = InstanceParameterKind::kType;
        param.value = type_param.targetType.getType().toString();
        param.is_overridden = type_param.isOverridden();
      } else {
        continue;
      }
      node.parameters.push_back(std::move(param));
    }
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
    const std::vector<std::string> &signal_paths) {
  slang::flat_hash_map<std::string_view, size_t> counts;
  for (const std::string &signal_path : signal_paths) {
    std::string_view inst = ParentPath(signal_path);
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
    const std::vector<std::string> &signal_paths, const std::vector<PartitionRecord> &parts) {
  std::vector<std::vector<size_t>> buckets(parts.size());
  if (parts.empty()) {
    buckets.resize(1);
    for (size_t i = 0; i < signal_paths.size(); ++i)
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

  for (size_t i = 0; i < signal_paths.size(); ++i) {
    const std::string &sig = signal_paths[i];
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

bool SaveGraphDb(const std::string &db_path, std::vector<SignalCompileItem> &signals,
                 std::vector<std::string> &signal_paths, const slang::SourceManager &sm,
                 const TraceDb &hier_db, const std::vector<std::vector<size_t>> *buckets,
                 size_t &signal_count,
                 CompileContext &compile_ctx, bool low_mem, CompileLogger *logger) {
  using Clock = std::chrono::steady_clock;
  auto fmt_seconds = [](const Clock::time_point &start, const Clock::time_point &end) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(3)
       << std::chrono::duration<double>(end - start).count();
    return os.str();
  };
  auto fmt_duration = [](double seconds) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(3) << seconds;
    return os.str();
  };
  auto elapsed_seconds = [](const Clock::time_point &start, const Clock::time_point &end) {
    return std::chrono::duration<double>(end - start).count();
  };
  const bool profile_save_graph = (std::getenv("RTL_TRACE_SAVE_GRAPH_PROFILE") != nullptr);
  g_build_prof.on = profile_save_graph;

  const auto t_total_start = Clock::now();
  if (logger != nullptr) {
    logger->Log("save_graph_db: begin keys=" + std::to_string(signals.size()) +
                " hier_nodes=" + std::to_string(hier_db.hierarchy.size()));
  }

  GraphDb graph;
  slang::flat_hash_map<std::string_view, uint32_t> string_index;
  // The load / driver / assignment-lhs reverse-ref tables are derived after the build loop from
  // graph.endpoints + graph.signal_refs (see build_path_refs below). Only assignment-lhs refs inferred
  // from assignment text are collected here: they intern new strings, which must happen in loop order.
  std::vector<std::pair<uint32_t, uint32_t>> inferred_lhs_refs;  // (lhs path id, signal id)
  // Compacted global nets. A sink is stored as a graph string id when its path is already interned,
  // else as kPooledSink | index into sink_pool (one copy per distinct path, interned at finalize in the
  // same order as before, so the DB does not change).
  constexpr uint32_t kPooledSink = 0x80000000u;
  struct CompactGlobalNet {
    std::string category;
    std::vector<uint32_t> sinks;
  };
  slang::flat_hash_map<std::string, CompactGlobalNet> compact_global_nets;
  std::deque<std::string> sink_pool;  // stable addresses: sink_pool_index holds views into it
  slang::flat_hash_map<std::string_view, uint32_t> sink_pool_index;
  size_t compact_sink_count = 0;
  TraceCompileCache trace_cache;
  {
    // Per-body trace indexes are pure functions of the (already bound) body, so the cache size only trades
    // memory against rebuild time; it never changes the DB. Lumion has 707k distinct bodies but rebuilt
    // 3.07M indexes with the old limit of 256 (high-fanout nets sweep thousands of bodies and flush the
    // cache); 4096 cuts that to 1.94M for ~+0.45 GB RSS (16384: 1.39M for ~+0.8 GB).
    // RTL_TRACE_BODY_CACHE=<n> overrides.
    size_t limit = low_mem ? 4u : 4096u;
    if (!low_mem) {
      const char *v = std::getenv("RTL_TRACE_BODY_CACHE");
      if (v != nullptr && *v != '\0') limit = std::max<size_t>(1, std::strtoull(v, nullptr, 10));
    }
    trace_cache.body_cache_limit = limit;
  }
  EndpointMergeScratch merge_scratch;

  // Reserve pool to guarantee stable string_view keys — must not reallocate.
  // Heuristic: ~1 unique string per signal + ~0.5 per endpoint for file/path/direction.
  const size_t estimated_strings = signals.size() * 2 + hier_db.hierarchy.size();
  // graph.strings must stay an upper bound (string_view keys into it); it is a plain vector, so the
  // over-reservation is untouched virtual memory.
  graph.strings.reserve(estimated_strings);
  // Untouched-virtual reservations for the big append-only vectors so they (almost) never reallocate:
  // a doubling realloc copies the touched half (+~1 GB transient on Lumion: 21.6M endpoints, 5.7 endpoints
  // per signal) and leaves the old buffer to be purged. RTL_TRACE_ENDPOINTS_PER_SIGNAL overrides (default 8).
  {
    const char *eps = std::getenv("RTL_TRACE_ENDPOINTS_PER_SIGNAL");
    const size_t per_sig = (eps != nullptr && *eps != '\0') ? std::max<size_t>(1, std::strtoull(eps, nullptr, 10)) : 8;
    graph.endpoints.reserve(signals.size() * per_sig);
    graph.signal_refs.reserve(signals.size() * per_sig * 3);
  }
  // string_index is a hash table (touches all its pages once populated), so size it to the strings that
  // will really be interned: one per signal, one per hierarchy node, plus a slack for file/text strings.
  // Under-estimating only costs a rehash.
  string_index.reserve(signals.size() + hier_db.hierarchy.size() + signals.size() / 16 + 4096);

  auto intern = [&](std::string_view sv) -> uint32_t {
    return InternString(sv, graph.strings, string_index);
  };
  auto append_signal_refs = [&](const std::vector<uint32_t> &signal_ids,
                                const std::vector<std::string> &signals,
                                uint32_t &begin, uint32_t &count) {
    begin = static_cast<uint32_t>(graph.signal_refs.size());
#ifndef NDEBUG
    assert(AreSortedUniqueSignalIdsByPath(signal_ids, graph));
    assert(AreSortedUniqueValues(signals));
#endif
    // Precondition: both inputs are individually sorted and deduplicated by
    // signal path text so this can do a linear merge without extra copies.
    if (signals.empty()) {
      graph.signal_refs.insert(graph.signal_refs.end(), signal_ids.begin(), signal_ids.end());
      count = static_cast<uint32_t>(signal_ids.size());
      return;
    }
    if (signal_ids.empty()) {
      for (const std::string &sig : signals)
        graph.signal_refs.push_back(intern(sig));
      count = static_cast<uint32_t>(signals.size());
      return;
    }
    size_t id_index = 0;
    size_t path_index = 0;
    while (id_index < signal_ids.size() || path_index < signals.size()) {
      bool take_id = false;
      if (id_index < signal_ids.size()) {
        if (path_index == signals.size()) {
          take_id = true;
        } else {
          const std::string &id_path = graph.strings[signal_ids[id_index]];
          if (id_path <= signals[path_index]) take_id = true;
        }
      }
      if (take_id) {
        const uint32_t id = signal_ids[id_index++];
        graph.signal_refs.push_back(id);
        while (path_index < signals.size() && graph.strings[id] == signals[path_index]) ++path_index;
      } else {
        graph.signal_refs.push_back(intern(signals[path_index++]));
      }
    }
    count = static_cast<uint32_t>(graph.signal_refs.size() - begin);
  };
  auto append_endpoint = [&](const EndpointRecord &e) {
    GraphEndpointRecord ge;
    ge.path_str_id = (e.path_id != std::numeric_limits<uint32_t>::max())
                         ? e.path_id
                         : intern(e.path);
    ge.file_str_id = intern(e.file);
    ge.direction_str_id = e.direction.empty() ? std::numeric_limits<uint32_t>::max() : intern(e.direction);
    ge.bit_map_str_id = e.bit_map.empty() ? std::numeric_limits<uint32_t>::max() : intern(e.bit_map);
    ge.line = static_cast<uint32_t>(std::max(e.line, 0));
    ge.assignment_start = e.assignment_start;
    ge.assignment_end = e.assignment_end;
    append_signal_refs(e.lhs_signal_ids, e.lhs_signals, ge.lhs_begin, ge.lhs_count);
    append_signal_refs(e.rhs_signal_ids, e.rhs_signals, ge.rhs_begin, ge.rhs_count);
    ge.kind = (e.kind == EndpointKind::kPort) ? 1u : 0u;
    ge.bit_map_approximate = e.bit_map_approximate ? 1u : 0u;
    ge.has_assignment_range = e.has_assignment_range ? 1u : 0u;
    graph.endpoints.push_back(ge);
  };

  // Pre-intern signal path names and build Symbol* → path_id reverse index.
  // This lets ResolveTraceResult skip getHierarchicalPath() for known symbols.
  graph.signals.resize(signals.size());
  slang::flat_hash_map<const slang::ast::Symbol *, uint32_t> symbol_path_ids;
  symbol_path_ids.reserve(signals.size());
  for (size_t i = 0; i < signals.size(); ++i) {
    graph.signals[i].name_str_id = intern(signal_paths[i]);
    // Member signals reuse the parent's Symbol*, so they must NOT be inserted
    // into symbol_path_ids — otherwise the last member overwrites the parent's
    // path, corrupting LHS/RHS resolution for all sibling endpoints.
    if (signals[i].sym != nullptr && signals[i].parent_signal_idx == std::numeric_limits<uint32_t>::max()) {
      symbol_path_ids[signals[i].sym] = graph.signals[i].name_str_id;
    }
    // Level 2: struct member metadata
    graph.signals[i].parent_signal_id = signals[i].parent_signal_idx;
    graph.signals[i].member_bit_offset = static_cast<uint32_t>(signals[i].member_bit_offset);
    graph.signals[i].member_bit_width = static_cast<uint32_t>(signals[i].member_bit_width);
  }

  LogMemPhase("SaveGraphDb:AfterPreIntern strings=" + std::to_string(graph.strings.size()) +
              " signals=" + std::to_string(graph.signals.size()));

  // Signals of slang-skipped instance bodies are traced on their canonical body and the paths are translated
  // back (db/CanonicalBodies.inc), so skipped bodies are never bound. RTL_TRACE_CANONICAL_BODIES=0 selects the
  // per-body binding baseline instead (same DB; fallback and A/B checks).
  const char *canonical_env = std::getenv("RTL_TRACE_CANONICAL_BODIES");
  const bool use_canonical_bodies =
      canonical_env == nullptr || !(canonical_env[0] == '0' && canonical_env[1] == '\0');
  std::unique_ptr<CanonicalTracer> canonical_tracer;
  if (use_canonical_bodies) {
    canonical_tracer = std::make_unique<CanonicalTracer>(sm, trace_cache, compile_ctx, symbol_path_ids,
                                                         graph.strings, string_index);
  }
  auto build_signal_record = [&](const slang::ast::Symbol *sym) -> SignalRecord {
    if (canonical_tracer) return canonical_tracer->Build(sym);
    return BuildSignalRecord(sym, sm, trace_cache, compile_ctx, &symbol_path_ids);
  };

  auto sort_bucket_for_locality = [&](std::vector<size_t> &bucket) {
    std::sort(bucket.begin(), bucket.end(), [&](size_t lhs, size_t rhs) {
      const std::string_view lhs_parent = ParentPath(signal_paths[lhs]);
      const std::string_view rhs_parent = ParentPath(signal_paths[rhs]);
      if (lhs_parent != rhs_parent) return lhs_parent < rhs_parent;
      return signal_paths[lhs] < signal_paths[rhs];
    });
  };

  std::vector<std::vector<size_t>> processing_buckets;
  if (buckets == nullptr || buckets->empty()) {
    processing_buckets.resize(1);
    std::vector<size_t> &default_bucket = processing_buckets[0];
    default_bucket.reserve(signals.size());
    for (size_t i = 0; i < signals.size(); ++i)
      default_bucket.push_back(i);
  } else {
    processing_buckets = *buckets;
  }
  for (std::vector<size_t> &bucket : processing_buckets) {
    sort_bucket_for_locality(bucket);
  }
  buckets = &processing_buckets;
  LogMemPhase("SaveGraphDb:AfterBucketSort buckets=" + std::to_string(processing_buckets.size()));

  // The signal paths are now duplicated in graph.strings (graph.signals[i].name_str_id) and the bucket
  // sort above was their last reader. Release the compile-side path vector as a whole (~1 GB on Lumion:
  // string headers + heap); the build loop reads paths back from graph.strings (a reserved vector,
  // never reallocated). RTL_TRACE_KEEP_SIGNAL_PATHS=1 keeps it (A/B measurement only).
  const bool release_signal_paths = !EnvFlagEnabled("RTL_TRACE_KEEP_SIGNAL_PATHS");
  if (release_signal_paths) {
    std::vector<std::string>().swap(signal_paths);
    LogMemPhase("SaveGraphDb:AfterReleaseSignalPaths");
  }

  // --- RTL_TRACE_MEM_PROGRESS=1: periodic memory/size progress inside the main loop ---
  const bool mem_progress = EnvFlagEnabled("RTL_TRACE_MEM_PROGRESS");
  // Defaults: a progress line every 100k signals, and whenever one signal yields >100k loads.
  // Overridable (mainly for testing on small designs) with
  // RTL_TRACE_MEM_PROGRESS_EVERY=<n> and RTL_TRACE_MEM_PROGRESS_BIG_LOADS=<n>.
  auto env_size = [](const char *name, size_t dflt) {
    const char *v = std::getenv(name);
    if (v == nullptr || *v == '\0') return dflt;
    char *end = nullptr;
    const unsigned long long x = std::strtoull(v, &end, 10);
    return (end != v && *end == '\0' && x > 0) ? static_cast<size_t>(x) : dflt;
  };
  const size_t kMemProgressEvery = env_size("RTL_TRACE_MEM_PROGRESS_EVERY", 100000);
  const size_t kMemProgressBigLoads = env_size("RTL_TRACE_MEM_PROGRESS_BIG_LOADS", 100000);
  size_t mem_progress_done = 0;        // signals through BuildSignalRecord (members excluded)
  size_t compact_sink_bytes = 0;       // total bytes of sink path strings in compact_global_nets
  size_t peak_loads = 0;               // largest rec.loads.size() seen (before compaction/merge)
  std::string peak_loads_path;
  long peak_rss_before_kb = 0, peak_rss_after_build_kb = 0, peak_rss_after_emit_kb = 0;
  auto cap_str = [](size_t size, size_t cap) {
    return std::to_string(size) + "/" + std::to_string(cap);
  };
  auto mem_progress_line = [&](const char *tag, const std::string &extra) {
    size_t trace_results = 0;
    size_t body_index_ready = 0;
    for (const auto &kv : trace_cache.body_caches) {
      const PerBodyTraceCache &bc = *kv.second;
      if (bc.body_trace_index_ready) ++body_index_ready;
      for (const auto &d : bc.body_trace_index.drivers) trace_results += d.second.size();
      for (const auto &l : bc.body_trace_index.loads) trace_results += l.second.size();
    }
    std::cout << "[Memory] progress " << tag << " signals_done=" << mem_progress_done
              << " rss=" << GetCurrentRSSKB() / 1024 << "MB peak_rss=" << GetMaxRSSMB() << "MB"
              << " endpoints(size/cap)=" << cap_str(graph.endpoints.size(), graph.endpoints.capacity())
              << " signal_refs=" << cap_str(graph.signal_refs.size(), graph.signal_refs.capacity())
              << " strings=" << cap_str(graph.strings.size(), graph.strings.capacity())
              << " inferred_lhs_refs=" << cap_str(inferred_lhs_refs.size(), inferred_lhs_refs.capacity())
              << " body_caches=" << trace_cache.body_caches.size() << " (index_ready=" << body_index_ready
              << ") trace_results=" << trace_results
              << " compact_global_nets=" << compact_global_nets.size()
              << " compact_sink_bytes=" << compact_sink_bytes << " compact_sinks=" << compact_sink_count
              << " pooled_sinks=" << sink_pool.size() << extra << "\n";
    std::cout.flush();
  };

  // Exact live-byte estimate of every long-lived accumulator (RTL_TRACE_MEM_PROGRESS=1).
  // Static inputs (compile-side `signals`, hierarchy db, buckets) are measured once.
  double acct_static_bytes = 0.0;
  bool acct_static_done = false;
  auto mem_accounting_line = [&](const char *tag) {
    if (!acct_static_done) {
      acct_static_done = true;
      double sig_tab = static_cast<double>(signals.capacity()) * sizeof(SignalCompileItem) +
                       static_cast<double>(signal_paths.capacity()) * sizeof(std::string);
      double sig_heap = 0;
      for (const std::string &p : signal_paths) sig_heap += static_cast<double>(StrHeap(p));
      double bucket_b = 0;
      for (const auto &b : processing_buckets) bucket_b += static_cast<double>(b.capacity()) * sizeof(size_t);
      double hier_tab = FlatMapTableBytes(hier_db.hierarchy);
      double hier_heap = 0;
      for (const auto &kv : hier_db.hierarchy) {
        hier_heap += static_cast<double>(StrHeap(kv.first) + StrHeap(kv.second.module) + StrHeap(kv.second.source_file));
        hier_heap += static_cast<double>(kv.second.parameters.capacity()) * sizeof(InstanceParameterRecord);
        for (const auto &pr : kv.second.parameters) hier_heap += static_cast<double>(StrHeap(pr.name) + StrHeap(pr.value));
        hier_heap += static_cast<double>(kv.second.children.capacity()) * sizeof(std::string);
        for (const auto &c : kv.second.children) hier_heap += static_cast<double>(StrHeap(c));
      }
      acct_static_bytes = sig_tab + sig_heap + bucket_b + hier_tab + hier_heap;
      std::cout << "[Memory] acct static compile-signals(table=" << Mb1(sig_tab) << "MB path_heap=" << Mb1(sig_heap)
                << "MB n=" << signals.size() << " cap=" << signals.capacity() << " sizeof=" << sizeof(SignalCompileItem)
                << ") buckets=" << Mb1(bucket_b) << "MB hier_db(table=" << Mb1(hier_tab) << "MB heap=" << Mb1(hier_heap)
                << "MB nodes=" << hier_db.hierarchy.size() << " bucket_count=" << hier_db.hierarchy.bucket_count()
                << " slot=" << sizeof(std::pair<const std::string, HierNodeRecord>) << ")\n";
    }
    const double ep_live = static_cast<double>(graph.endpoints.size()) * sizeof(GraphEndpointRecord);
    const double ep_cap = static_cast<double>(graph.endpoints.capacity()) * sizeof(GraphEndpointRecord);
    const double sr_cap = static_cast<double>(graph.signal_refs.capacity()) * sizeof(uint32_t);
    double str_heap = 0;
    for (const std::string &x : graph.strings) str_heap += static_cast<double>(StrHeap(x));
    const double str_tab = static_cast<double>(graph.strings.capacity()) * sizeof(std::string);
    const double sidx = FlatMapTableBytes(string_index);
    const double spid = FlatMapTableBytes(symbol_path_ids);
    const double gsig = static_cast<double>(graph.signals.capacity()) * sizeof(GraphSignalRecord);
    const double refs =
        static_cast<double>(inferred_lhs_refs.capacity()) * sizeof(std::pair<uint32_t, uint32_t>);
    double cg = FlatMapTableBytes(compact_global_nets) + FlatMapTableBytes(sink_pool_index) +
                static_cast<double>(sink_pool.size()) * sizeof(std::string);
    for (const auto &kv : compact_global_nets) {
      cg += static_cast<double>(StrHeap(kv.first) + StrHeap(kv.second.category));
      cg += static_cast<double>(kv.second.sinks.capacity()) * sizeof(uint32_t);
    }
    for (const std::string &sk : sink_pool) cg += static_cast<double>(StrHeap(sk));
    const double total = ep_cap + sr_cap + str_heap + str_tab + sidx + spid + gsig + refs + cg + acct_static_bytes;
    std::cout << "[Memory] acct " << tag << " signals_done=" << mem_progress_done
              << " endpoints(live/cap)=" << Mb1(ep_live) << "/" << Mb1(ep_cap) << "MB signal_refs_cap=" << Mb1(sr_cap)
              << "MB strings(table=" << Mb1(str_tab) << "MB heap=" << Mb1(str_heap) << "MB) string_index=" << Mb1(sidx)
              << "MB(bc=" << string_index.bucket_count() << ") symbol_path_ids=" << Mb1(spid)
              << "MB graph.signals=" << Mb1(gsig) << "MB ref_pairs_cap=" << Mb1(refs) << "MB compact_global=" << Mb1(cg)
              << "MB static(compile signals+hier+buckets)=" << Mb1(acct_static_bytes) << "MB => accounted_total="
              << Mb1(total) << "MB rss=" << GetCurrentRSSKB() / 1024 << "MB unaccounted="
              << static_cast<long>(GetCurrentRSSKB() / 1024 - MB(total)) << "MB\n";
    LogProcMemDetail(tag);
  };
  if (mem_progress) {
    mem_accounting_line("pre_loop");
    if (EnvFlagEnabled("RTL_TRACE_MI_STATS")) LogMiStats("BeforeBuildLoop");
  }

  const auto t_build_start = Clock::now();
  double t_build_signal_record_s = 0.0;
  double t_compact_global_s = 0.0;
  double t_merge_s = 0.0;
  double t_emit_driver_s = 0.0;
  double t_emit_load_s = 0.0;
  double t_cache_clear_s = 0.0;
  size_t build_driver_endpoint_count = 0;
  size_t build_load_endpoint_count = 0;
  size_t inferred_assignment_lhs_count = 0;
  signal_count = 0;
  for (size_t bucket_index = 0; bucket_index < buckets->size(); ++bucket_index) {
    const std::vector<size_t> &bucket = (*buckets)[bucket_index];
    if (logger != nullptr && buckets->size() > 1) {
      logger->Log("save_graph_db: partition_begin index=" + std::to_string(bucket_index) +
                  " signals=" + std::to_string(bucket.size()));
    }
    for (size_t sig_id : bucket) {
      const SignalCompileItem &item = signals[sig_id];
      if (item.sym == nullptr || !IsTraceable(item.sym)) continue;
      const std::string &item_path = graph.strings[graph.signals[sig_id].name_str_id];
      // Level 2: struct member signals derive endpoints from parent at query time
      if (item.parent_signal_idx != std::numeric_limits<uint32_t>::max()) {
        ++signal_count;
        continue;
      }

      const auto t_signal_record_start = profile_save_graph ? Clock::now() : Clock::time_point{};
      const long mem_rss_before_kb = mem_progress ? GetCurrentRSSKB() : 0;
      SignalRecord rec = build_signal_record(item.sym);
      if (profile_save_graph) t_build_signal_record_s += elapsed_seconds(t_signal_record_start, Clock::now());
      bool mem_new_peak = false;
      if (mem_progress) {
        const long mem_rss_after_kb = GetCurrentRSSKB();
        if (rec.loads.size() > peak_loads) {
          mem_new_peak = true;
          peak_loads = rec.loads.size();
          peak_loads_path = item_path;
          peak_rss_before_kb = mem_rss_before_kb;
          peak_rss_after_build_kb = mem_rss_after_kb;
        }
        if (rec.loads.size() > kMemProgressBigLoads) {
          mem_progress_line("big_signal",
                            " signal=" + item_path + " loads=" + std::to_string(rec.loads.size()) +
                                " drivers=" + std::to_string(rec.drivers.size()) +
                                " rss_before=" + std::to_string(mem_rss_before_kb / 1024) +
                                "MB rss_after_build=" + std::to_string(mem_rss_after_kb / 1024) + "MB");
        }
      }
      const auto t_compact_global_start = profile_save_graph ? Clock::now() : Clock::time_point{};
      if (ShouldCompactGlobalNet(item_path, rec.loads.size())) {
        const std::vector<std::string_view> sink_paths = ExtractCompactSinkPaths(item_path, graph, rec.loads);
        if (!sink_paths.empty()) {
          CompactGlobalNet g;
          g.category = ClassifyGlobalNetCategory(item_path);
          g.sinks.reserve(sink_paths.size());
          for (const std::string_view sink : sink_paths) {
            if (mem_progress) compact_sink_bytes += sink.size();
            if (const auto it = string_index.find(sink); it != string_index.end()) {
              if ((it->second & kPooledSink) != 0) return false;  // > 2^31 strings: cannot tag
              g.sinks.push_back(it->second);
              continue;
            }
            uint32_t pool_idx = 0;
            if (const auto pit = sink_pool_index.find(sink); pit != sink_pool_index.end()) {
              pool_idx = pit->second;
            } else {
              if (sink_pool.size() >= kPooledSink) return false;
              pool_idx = static_cast<uint32_t>(sink_pool.size());
              // Key on the pool's own copy: `sink` may point into rec.loads, which is about to go.
              sink_pool.emplace_back(sink);
              sink_pool_index.emplace(std::string_view(sink_pool.back()), pool_idx);
            }
            g.sinks.push_back(kPooledSink | pool_idx);
          }
          compact_sink_count += g.sinks.size();
          compact_global_nets.emplace(item_path, std::move(g));
          rec.loads.clear();
        }
      }
      if (profile_save_graph) t_compact_global_s += elapsed_seconds(t_compact_global_start, Clock::now());
      const auto t_merge_start = profile_save_graph ? Clock::now() : Clock::time_point{};
      MergeEndpointBitRangesInPlace(rec.drivers, merge_scratch);
      MergeEndpointBitRangesInPlace(rec.loads, merge_scratch);
      if (profile_save_graph) t_merge_s += elapsed_seconds(t_merge_start, Clock::now());

      GraphSignalRecord &gs = graph.signals[sig_id];
      gs.driver_begin = static_cast<uint32_t>(graph.endpoints.size());
      gs.driver_count = static_cast<uint32_t>(rec.drivers.size());
      build_driver_endpoint_count += rec.drivers.size();
      const auto t_emit_driver_start = profile_save_graph ? Clock::now() : Clock::time_point{};
      for (const EndpointRecord &e : rec.drivers) append_endpoint(e);
      if (profile_save_graph) t_emit_driver_s += elapsed_seconds(t_emit_driver_start, Clock::now());
      gs.load_begin = static_cast<uint32_t>(graph.endpoints.size());
      gs.load_count = static_cast<uint32_t>(rec.loads.size());
      build_load_endpoint_count += rec.loads.size();
      const auto t_emit_load_start = profile_save_graph ? Clock::now() : Clock::time_point{};
      for (const EndpointRecord &e : rec.loads) {
        append_endpoint(e);
        // A load without LHS signals (ge.lhs_count == 0) gets its assignment-lhs refs from the assignment
        // text; interning them here keeps the string-id order. Loads with LHS signals are covered by
        // build_path_refs after the loop (their LHS ids are the endpoint's signal_refs range).
        if (e.lhs_signal_ids.empty() && e.lhs_signals.empty() && !e.assignment_text.empty()) {
          const std::vector<std::string> inferred_lhs_paths =
              InferAssignmentLhsPathsFromText(e.path, e.assignment_text);
          ++inferred_assignment_lhs_count;
          for (const std::string &lhs : inferred_lhs_paths) {
            if (!lhs.empty()) inferred_lhs_refs.push_back({intern(lhs), static_cast<uint32_t>(sig_id)});
          }
        }
      }
      if (profile_save_graph) t_emit_load_s += elapsed_seconds(t_emit_load_start, Clock::now());
      ++signal_count;
      TrimTraceCompileCache(trace_cache);
      if (mem_progress) {
        ++mem_progress_done;
        if (mem_new_peak) peak_rss_after_emit_kb = GetCurrentRSSKB();
        if (mem_progress_done % kMemProgressEvery == 0) {
          mem_progress_line("periodic", "");
          mem_accounting_line("periodic");
          if (EnvFlagEnabled("RTL_TRACE_MI_STATS")) LogMiCensus("periodic");
        }
      }
    }
    if (buckets->size() > 1) {
      const auto t_cache_clear_start = profile_save_graph ? Clock::now() : Clock::time_point{};
      ClearTraceCompileCache(trace_cache);
      if (profile_save_graph) t_cache_clear_s += elapsed_seconds(t_cache_clear_start, Clock::now());
    }
  }
  const auto t_cache_clear_start = profile_save_graph ? Clock::now() : Clock::time_point{};
  ClearTraceCompileCache(trace_cache);
  if (profile_save_graph) t_cache_clear_s += elapsed_seconds(t_cache_clear_start, Clock::now());
  const auto t_build_end = Clock::now();
  LogMemPhase("SaveGraphDb:AfterBuildLoop endpoints=" + std::to_string(graph.endpoints.size()) +
              " signal_refs=" + std::to_string(graph.signal_refs.size()));
  if (canonical_tracer) canonical_tracer->Report();
  CanonStatsReport();
  if (mem_progress) {
    mem_progress_line("build_done", "");
    mem_accounting_line("build_done");
    if (EnvFlagEnabled("RTL_TRACE_MI_STATS")) LogMiStats("AfterBuildLoop");
    std::cout << "[Memory] progress peak_loads_signal=" << peak_loads_path << " loads=" << peak_loads
              << " rss_before=" << peak_rss_before_kb / 1024 << "MB rss_after_build="
              << peak_rss_after_build_kb / 1024 << "MB rss_after_emit=" << peak_rss_after_emit_kb / 1024
              << "MB delta_build=" << (peak_rss_after_build_kb - peak_rss_before_kb) / 1024
              << "MB delta_total=" << (peak_rss_after_emit_kb - peak_rss_before_kb) / 1024 << "MB\n";
  }
  if (logger != nullptr) {
    logger->Log("save_graph_db: build_graph done elapsed_s=" +
                fmt_seconds(t_build_start, t_build_end) +
                " strings=" + std::to_string(graph.strings.size()) +
                " endpoints=" + std::to_string(graph.endpoints.size()));
    if (profile_save_graph) {
      logger->Log("save_graph_db: build_graph phases signal_record_s=" +
                  fmt_duration(t_build_signal_record_s) +
                  " compact_global_s=" + fmt_duration(t_compact_global_s) +
                  " merge_s=" + fmt_duration(t_merge_s) +
                  " emit_drivers_s=" + fmt_duration(t_emit_driver_s) +
                  " emit_loads_s=" + fmt_duration(t_emit_load_s) +
                  " cache_clear_s=" + fmt_duration(t_cache_clear_s));
      logger->Log("save_graph_db: build_graph signal_record breakdown (nested/overlapping) index_build_s=" +
                  fmt_duration(g_build_prof.index_build_s) + " bodies_built=" +
                  std::to_string(g_build_prof.bodies_built) + " bodies_distinct=" +
                  std::to_string(g_build_prof.bodies_seen.size()) + " bodies_rebuilt=" +
                  std::to_string(g_build_prof.bodies_rebuilt) +
                  " port_follow_s=" + fmt_duration(g_build_prof.port_follow_s) +
                  " resolve_s=" + fmt_duration(g_build_prof.resolve_s) +
                  " symbol_path_less_s=" + fmt_duration(g_build_prof.path_less_s) +
                  " symbol_path_sorted_elems=" + std::to_string(g_build_prof.path_less_calls));
      logger->Log("save_graph_db: build_graph counts driver_endpoints=" +
                  std::to_string(build_driver_endpoint_count) +
                  " load_endpoints=" + std::to_string(build_load_endpoint_count) +
                  " signal_refs=" + std::to_string(graph.signal_refs.size()) +
                  " inferred_assignment_lhs=" + std::to_string(inferred_assignment_lhs_count));
    }
  }

  // Reverse-ref tables (path id -> sorted unique ids of the signals whose endpoints reference it),
  // derived from graph.endpoints / graph.signal_refs instead of (path, signal) pairs collected in the
  // build loop. Same result as sorting + deduplicating the pairs, computed as a counting sort over the
  // path ids: signals are visited in ascending id order, so every path's bucket fills in ascending
  // signal order and only adjacent duplicates need dropping. One table at a time.
  std::vector<uint32_t> ref_cursor;
  auto build_path_refs = [&](const auto &for_each_ref, std::vector<GraphPathRefRange> &ranges,
                             std::vector<uint32_t> &flat) {
    const size_t num_paths = graph.strings.size();
    ref_cursor.assign(num_paths, 0);
    size_t total = 0;
    for_each_ref([&](uint32_t path_id, uint32_t) {
      ++ref_cursor[path_id];
      ++total;
    });
    if (total == 0) return;
    uint32_t running = 0;
    for (uint32_t &c : ref_cursor) {
      const uint32_t n = c;
      c = running;
      running += n;
    }
    std::vector<uint32_t> ids(total);
    for_each_ref([&](uint32_t path_id, uint32_t sig) { ids[ref_cursor[path_id]++] = sig; });
    // ref_cursor[p] is now the end of bucket p (and the start of bucket p + 1). Compact in place.
    size_t out = 0;
    uint32_t bucket_begin = 0;
    for (size_t p = 0; p < num_paths; ++p) {
      const uint32_t bucket_end = ref_cursor[p];
      if (bucket_begin == bucket_end) continue;
      GraphPathRefRange range;
      range.path_str_id = static_cast<uint32_t>(p);
      range.begin = static_cast<uint32_t>(out);
      for (uint32_t i = bucket_begin; i < bucket_end; ++i) {
        if (i == bucket_begin || ids[i] != ids[i - 1]) ids[out++] = ids[i];
      }
      range.count = static_cast<uint32_t>(out - range.begin);
      ranges.push_back(range);
      bucket_begin = bucket_end;
    }
    ids.resize(out);
    ids.shrink_to_fit();
    flat.swap(ids);
  };
  constexpr uint32_t kNoPath = std::numeric_limits<uint32_t>::max();
  const uint32_t num_graph_signals = static_cast<uint32_t>(graph.signals.size());
  const auto t_finalize_refs_start = profile_save_graph ? Clock::now() : Clock::time_point{};
  build_path_refs(
      [&](const auto &emit) {
        for (uint32_t sig = 0; sig < num_graph_signals; ++sig) {
          const GraphSignalRecord &gs = graph.signals[sig];
          for (uint32_t k = 0; k < gs.load_count; ++k) {
            const uint32_t path_id = graph.endpoints[gs.load_begin + k].path_str_id;
            if (path_id != kNoPath) emit(path_id, sig);
          }
        }
      },
      graph.load_ref_ranges, graph.load_ref_signal_ids);
  LogMemPhase("SaveGraphDb:AfterFinalizeLoadRefs unique_refs=" +
              std::to_string(graph.load_ref_signal_ids.size()));
  build_path_refs(
      [&](const auto &emit) {
        for (uint32_t sig = 0; sig < num_graph_signals; ++sig) {
          const GraphSignalRecord &gs = graph.signals[sig];
          for (uint32_t k = 0; k < gs.driver_count; ++k) {
            const uint32_t path_id = graph.endpoints[gs.driver_begin + k].path_str_id;
            if (path_id != kNoPath) emit(path_id, sig);
          }
        }
      },
      graph.driver_ref_ranges, graph.driver_ref_signal_ids);
  LogMemPhase("SaveGraphDb:AfterFinalizeDriverRefs unique_refs=" +
              std::to_string(graph.driver_ref_signal_ids.size()));
  // Assignment-lhs refs of a load: its LHS signal refs (signal ids plus interned LHS path strings;
  // empty paths never were refs), or, for a load without LHS signals, the refs inferred from the
  // assignment text in the loop (merged in here in signal order).
  std::sort(inferred_lhs_refs.begin(), inferred_lhs_refs.end(),
            [](const std::pair<uint32_t, uint32_t> &a, const std::pair<uint32_t, uint32_t> &b) {
              return a.second != b.second ? a.second < b.second : a.first < b.first;
            });
  build_path_refs(
      [&](const auto &emit) {
        size_t inferred = 0;
        for (uint32_t sig = 0; sig < num_graph_signals; ++sig) {
          const GraphSignalRecord &gs = graph.signals[sig];
          for (uint32_t k = 0; k < gs.load_count; ++k) {
            const GraphEndpointRecord &ge = graph.endpoints[gs.load_begin + k];
            for (uint32_t r = 0; r < ge.lhs_count; ++r) {
              const uint32_t lhs_id = graph.signal_refs[ge.lhs_begin + r];
              if (!graph.strings[lhs_id].empty()) emit(lhs_id, sig);
            }
          }
          for (; inferred < inferred_lhs_refs.size() && inferred_lhs_refs[inferred].second == sig; ++inferred)
            emit(inferred_lhs_refs[inferred].first, sig);
        }
      },
      graph.assignment_lhs_ref_ranges, graph.assignment_lhs_ref_signal_ids);
  LogMemPhase("SaveGraphDb:AfterFinalizeAssignmentLhsRefs unique_refs=" +
              std::to_string(graph.assignment_lhs_ref_signal_ids.size()));
  const double t_finalize_refs_s =
      profile_save_graph ? elapsed_seconds(t_finalize_refs_start, Clock::now()) : 0.0;
  std::vector<uint32_t>().swap(ref_cursor);
  std::vector<std::pair<uint32_t, uint32_t>>().swap(inferred_lhs_refs);
  LogMemPhase("SaveGraphDb:AfterRefVectorSwaps");

  const auto t_build_hierarchy_start = profile_save_graph ? Clock::now() : Clock::time_point{};
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

    if (!it->second.parameters.empty()) {
      GraphPathRefRange range;
      range.path_str_id = gh.path_str_id;
      range.begin = static_cast<uint32_t>(graph.hierarchy_params.size());
      range.count = static_cast<uint32_t>(it->second.parameters.size());
      graph.hierarchy_param_ranges.push_back(range);
      for (const InstanceParameterRecord &param : it->second.parameters) {
        GraphInstanceParamRecord gp;
        gp.name_str_id = intern(param.name);
        gp.value_str_id = intern(param.value);
        gp.kind = param.kind == InstanceParameterKind::kValue ? 0 : 1;
        gp.is_local = param.is_local ? 1 : 0;
        gp.is_port = param.is_port ? 1 : 0;
        gp.is_overridden = param.is_overridden ? 1 : 0;
        graph.hierarchy_params.push_back(gp);
      }
    }
  }
  const double t_build_hierarchy_s =
      profile_save_graph ? elapsed_seconds(t_build_hierarchy_start, Clock::now()) : 0.0;
  LogMemPhase("SaveGraphDb:AfterHierarchy nodes=" + std::to_string(graph.hierarchy.size()) +
              " strings=" + std::to_string(graph.strings.size()));

  const auto t_build_global_nets_start = profile_save_graph ? Clock::now() : Clock::time_point{};
  std::vector<std::string> global_sources;
  global_sources.reserve(compact_global_nets.size());
  for (const auto &[path, _] : compact_global_nets)
    global_sources.push_back(path);
  std::sort(global_sources.begin(), global_sources.end());
  graph.global_nets.reserve(global_sources.size());
  for (const std::string &source : global_sources) {
    const auto it = compact_global_nets.find(source);
    if (it == compact_global_nets.end()) continue;
    GraphGlobalNetRecord gg;
    gg.source_path_str_id = intern(source);
    gg.category_str_id = it->second.category.empty() ? std::numeric_limits<uint32_t>::max()
                                                     : intern(it->second.category);
    gg.sink_begin = static_cast<uint32_t>(graph.global_sinks.size());
    gg.sink_count = static_cast<uint32_t>(it->second.sinks.size());
    for (const uint32_t sink : it->second.sinks) {
      // intern() of an already-interned path returns its id, so only pooled paths can add strings,
      // exactly where interning the std::string copies used to add them.
      graph.global_sinks.push_back((sink & kPooledSink) != 0 ? intern(sink_pool[sink & ~kPooledSink])
                                                              : sink);
    }
    graph.global_nets.push_back(gg);
  }
  const size_t pooled_sink_paths = sink_pool.size();
  sink_pool_index = {};
  std::deque<std::string>().swap(sink_pool);
  compact_global_nets = {};
  const double t_build_global_nets_s =
      profile_save_graph ? elapsed_seconds(t_build_global_nets_start, Clock::now()) : 0.0;
  LogMemPhase("SaveGraphDb:AfterGlobalNets nets=" + std::to_string(graph.global_nets.size()) +
              " sinks=" + std::to_string(graph.global_sinks.size()) +
              " pooled_sink_paths=" + std::to_string(pooled_sink_paths) +
              " strings=" + std::to_string(graph.strings.size()));

  const auto t_string_offsets_start = profile_save_graph ? Clock::now() : Clock::time_point{};
  std::vector<uint32_t> string_offsets;
  string_offsets.reserve(graph.strings.size() + 1);
  uint64_t total_str_bytes = 0;
  for (const std::string &s : graph.strings) {
    if (total_str_bytes > std::numeric_limits<uint32_t>::max()) return false;
    string_offsets.push_back(static_cast<uint32_t>(total_str_bytes));
    total_str_bytes += s.size();
  }
  if (total_str_bytes > std::numeric_limits<uint32_t>::max()) return false;
  string_offsets.push_back(static_cast<uint32_t>(total_str_bytes));
  string_index.clear();
  const double t_string_offsets_s =
      profile_save_graph ? elapsed_seconds(t_string_offsets_start, Clock::now()) : 0.0;
  LogMemPhase("SaveGraphDb:AfterStringOffsets string_bytes=" + std::to_string(total_str_bytes));

  if (logger != nullptr && profile_save_graph) {
    logger->Log("save_graph_db: finalize phases refs_s=" + fmt_duration(t_finalize_refs_s) +
                " hierarchy_s=" + fmt_duration(t_build_hierarchy_s) +
                " global_nets_s=" + fmt_duration(t_build_global_nets_s) +
                " string_offsets_s=" + fmt_duration(t_string_offsets_s));
  }

  GraphDbFileHeader header;
  std::memcpy(header.magic, kGraphDbMagic, sizeof(header.magic));
  header.string_count = graph.strings.size();
  header.string_blob_size = total_str_bytes;
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

  LogMemPhase("SaveGraphDb:BeforeWrite");
  const auto t_write_start = Clock::now();
  ScopedFileLock db_lock;
  if (!db_lock.Acquire(db_path)) return false;
  std::ofstream out(db_path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) return false;
  if (!WriteBinaryValue(out, header) || !WriteBinaryVector(out, string_offsets)) return false;
  for (const std::string &s : graph.strings) {
    out.write(s.data(), static_cast<std::streamsize>(s.size()));
    if (!out.good()) return false;
  }
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
  const uint64_t hierarchy_param_range_count = graph.hierarchy_param_ranges.size();
  const uint64_t hierarchy_param_count = graph.hierarchy_params.size();
  if (!WriteBinaryValue(out, hierarchy_param_range_count) ||
      !WriteBinaryVector(out, graph.hierarchy_param_ranges) ||
      !WriteBinaryValue(out, hierarchy_param_count) ||
      !WriteBinaryVector(out, graph.hierarchy_params)) {
    return false;
  }
  const auto t_write_end = Clock::now();
  LogMemPhase("SaveGraphDb:AfterWrite");
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

  for (const GraphPathRefRange &range : graph.hierarchy_param_ranges) {
    if (!ValidateGraphRange(range.begin, range.count, graph.hierarchy_params.size())) return false;
  }

  for (const GraphGlobalNetRecord &gg : graph.global_nets) {
    if (!ValidateGraphRange(gg.sink_begin, gg.sink_count, graph.global_sinks.size())) return false;
  }

  return true;
}

bool LoadGraphDb(const std::string &db_path, GraphDb &graph, TraceDb &compat_db) {
  ScopedFileLock db_lock;
  if (!db_lock.AcquireShared(db_path)) return false;

  std::ifstream in(db_path, std::ios::binary);
  if (!in.is_open()) return false;

  GraphDbFileHeader header;
  if (!ReadBinaryValue(in, header)) return false;
  if (std::memcmp(header.magic, kGraphDbMagic, sizeof(header.magic)) != 0) return false;
  if (header.version != 1 && header.version != 2 && header.version != 3 && header.version != 4 &&
      header.version != 5)
    return false;

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

  // Read signal records. v4 has 20-byte records (5 fields), v5 has 32-byte (8 fields).
  if (header.version <= 4) {
    struct GraphSignalRecordV4 {
      uint32_t name_str_id, driver_begin, driver_count, load_begin, load_count;
    };
    std::vector<GraphSignalRecordV4> v4_signals;
    if (!ReadBinaryVector(in, v4_signals, static_cast<size_t>(header.signal_count))) return false;
    graph.signals.resize(v4_signals.size());
    for (size_t i = 0; i < v4_signals.size(); ++i) {
      graph.signals[i].name_str_id = v4_signals[i].name_str_id;
      graph.signals[i].driver_begin = v4_signals[i].driver_begin;
      graph.signals[i].driver_count = v4_signals[i].driver_count;
      graph.signals[i].load_begin = v4_signals[i].load_begin;
      graph.signals[i].load_count = v4_signals[i].load_count;
      // parent_signal_id, member_bit_offset, member_bit_width stay at defaults (UINT32_MAX / 0)
    }
  } else {
    if (!ReadBinaryVector(in, graph.signals, static_cast<size_t>(header.signal_count))) return false;
  }
  if (!ReadBinaryVector(in, graph.endpoints, static_cast<size_t>(header.endpoint_count)) ||
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
  if (header.version >= 4) {
    uint64_t hierarchy_param_range_count = 0;
    uint64_t hierarchy_param_count = 0;
    if (!ReadBinaryValue(in, hierarchy_param_range_count) ||
        !ReadBinaryVector(in, graph.hierarchy_param_ranges,
                          static_cast<size_t>(hierarchy_param_range_count)) ||
        !ReadBinaryValue(in, hierarchy_param_count) ||
        !ReadBinaryVector(in, graph.hierarchy_params,
                          static_cast<size_t>(hierarchy_param_count))) {
      return false;
    }
  }
  if (!ValidateGraphDb(graph)) return false;

  compat_db.db_dir = std::filesystem::path(db_path).parent_path().string();
  compat_db.format_version = header.version;
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

  slang::flat_hash_map<uint32_t, size_t> hierarchy_param_index;
  hierarchy_param_index.reserve(graph.hierarchy_param_ranges.size());
  for (size_t i = 0; i < graph.hierarchy_param_ranges.size(); ++i)
    hierarchy_param_index.emplace(graph.hierarchy_param_ranges[i].path_str_id, i);

  for (const GraphHierarchyRecord &gh : graph.hierarchy) {
    const std::string &path = GraphString(graph, gh.path_str_id);
    auto &node = compat_db.hierarchy[path];
    node.module = GraphString(graph, gh.module_str_id);
    if (header.version >= 3 && gh.file_str_id != std::numeric_limits<uint32_t>::max()) {
      node.source_file = GraphString(graph, gh.file_str_id);
      node.source_line = gh.line;
    }
    auto param_it = hierarchy_param_index.find(gh.path_str_id);
    if (param_it != hierarchy_param_index.end()) {
      const GraphPathRefRange &range = graph.hierarchy_param_ranges[param_it->second];
      node.parameters.reserve(range.count);
      for (uint32_t i = 0; i < range.count; ++i) {
        const GraphInstanceParamRecord &gp = graph.hierarchy_params[range.begin + i];
        InstanceParameterRecord param;
        param.name = GraphString(graph, gp.name_str_id);
        param.value = GraphString(graph, gp.value_str_id);
        param.kind = gp.kind == 0 ? InstanceParameterKind::kValue : InstanceParameterKind::kType;
        param.is_local = gp.is_local != 0;
        param.is_port = gp.is_port != 0;
        param.is_overridden = gp.is_overridden != 0;
        node.parameters.push_back(std::move(param));
      }
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

  // Level 2: struct member signals derive endpoints from parent by bit-range filtering
  if (gs.parent_signal_id != std::numeric_limits<uint32_t>::max()) {
    const SignalRecord &parent = SessionSignalRecord(session, gs.parent_signal_id);
    int32_t hi = static_cast<int32_t>(gs.member_bit_offset + gs.member_bit_width - 1);
    int32_t lo = static_cast<int32_t>(gs.member_bit_offset);
    auto sel = std::make_pair(hi, lo);
    SignalRecord rec;
    for (const EndpointRecord &e : parent.drivers)
      if (EndpointMatchesSignalSelect(e, sel)) rec.drivers.push_back(e);
    for (const EndpointRecord &e : parent.loads)
      if (EndpointMatchesSignalSelect(e, sel)) rec.loads.push_back(e);
    return session.materialized_signal_records.emplace(id, std::move(rec)).first->second;
  }

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

bool HasBlockingCompileDiagnostics(slang::ast::Compilation &compilation,
                                   const slang::DiagnosticEngine &diagEngine,
                                   bool relax_defparam) {
  const slang::SourceManager &sm = *compilation.getSourceManager();
  for (const slang::Diagnostic &diag : compilation.getAllDiagnostics()) {
    const auto severity = diagEngine.getSeverity(diag.code, diag.location);
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

// Levenshtein distance identical to EditDistance() whenever the result is <= bound; otherwise
// returns some value > bound. `prev`/`cur` are caller-provided scratch rows (reused across calls).
static size_t BoundedEditDistance(const std::string &a, const std::string &b, size_t bound,
                                  std::vector<size_t> &prev, std::vector<size_t> &cur) {
  const size_t diff = a.size() > b.size() ? a.size() - b.size() : b.size() - a.size();
  if (diff > bound) return diff;
  prev.resize(b.size() + 1);
  cur.resize(b.size() + 1);
  for (size_t j = 0; j <= b.size(); ++j)
    prev[j] = j;
  for (size_t i = 1; i <= a.size(); ++i) {
    cur[0] = i;
    size_t row_min = cur[0];
    for (size_t j = 1; j <= b.size(); ++j) {
      const size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
      cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
      row_min = std::min(row_min, cur[j]);
    }
    // Values never decrease along an alignment path, so the final distance is >= any row minimum.
    if (row_min > bound) return row_min;
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


// Bump whenever the same sources and arguments produce different DB content, so `--incremental`
// never reuses a DB built with older semantics. .meta files without a SEMANTICS_EPOCH line are
// epoch 1. Epoch 2: endpoint bit-range merge fix (66c78ff).
constexpr int kCompileSemanticsEpoch = 2;

std::string ComputeCompileFingerprint(const std::vector<std::string> &passthrough_args) {
  std::vector<std::string> parts;
  parts.push_back("rtl_trace_compile_fingerprint_v1");
  parts.push_back("SEMANTICS_EPOCH:" + std::to_string(kCompileSemanticsEpoch));
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
               "[--format <text|json>] [--show-params]\n";
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
               "[--format <text|json>] [--show-params]\n";
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
  std::cout << "  whereis-instance --instance <hier.path> [--format <text|json>] [--show-params]\n";
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
  // Result is the first max(limit,1) names ordered by (edit distance, name) — as if every name
  // were scored and fully sorted — computed with a parallel bounded top-k. Once a worker holds
  // `keep` candidates, names that cannot beat its current worst are rejected early via a
  // bounded edit distance (exact whenever the distance is <= the bound).
  using Scored = std::pair<size_t, const std::string *>;
  auto scored_less = [](const Scored &a, const Scored &b) {
    if (a.first != b.first) return a.first < b.first;
    return *a.second < *b.second;
  };
  using Collector = TopKCollector<Scored, decltype(scored_less)>;
  const size_t keep = std::max<size_t>(limit, 1);
  const std::vector<const std::string *> &names = session.signal_names_by_id;
  const std::vector<Scored> top = ParallelTopK<Scored>(
      names.size(), keep, scored_less, [&](size_t begin, size_t end, Collector &out) {
        std::vector<size_t> prev, cur;
        for (size_t i = begin; i < end; ++i) {
          const std::string &name = *names[i];
          const size_t bound = out.Full() ? out.Worst().first : std::numeric_limits<size_t>::max();
          const size_t d = BoundedEditDistance(name, needle, bound, prev, cur);
          if (d <= bound) out.Push({d, names[i]});
        }
      });
  std::vector<std::string> out;
  out.reserve(top.size());
  for (const Scored &p : top) out.push_back(*p.second);
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
