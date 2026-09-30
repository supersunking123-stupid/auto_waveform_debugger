// CanonicalBodiesStats.inc — included by GraphDb.cc (TODO item 4 spike instrumentation).
//
// RTL_TRACE_CANONICAL_STATS=1 classifies every instance body the SaveGraphDb build loop touches:
//   class 0 "bound"   : body visited by slang elaboration (its expressions were bound before the loop),
//   class 1 "skip-root": instance whose body slang skipped because getCanonicalBody() points to an
//                        identical body (the root of a skipped subtree; its port connections are bound),
//   class 2 "skip-sub" : any instance inside a skipped subtree (nothing in it was bound by elaboration,
//                        not even its port connections).
// and attributes RSS growth (statm deltas) to per-body trace-index builds (by call path: the traced
// signal's own body vs. downward port follow), upward port-connection follows and whole signals.
// Pure instrumentation: never changes the DB.

struct CanonicalStats {
  bool on = false;
  bool init = false;
  int statm_fd = -1;
  long page_kb = 4;
  enum Reason { kStart = 0, kDownward = 1, kNumReasons = 2 };
  int reason = kStart;
  slang::flat_hash_map<const slang::ast::InstanceBodySymbol *, uint8_t> body_class;
  slang::flat_hash_set<const slang::ast::InstanceBodySymbol *> canonical_bodies;
  size_t n_inst[3] = {0, 0, 0};
  size_t n_iface_skip_roots = 0;  // skip-roots whose canonical body has interface ports
  size_t builds[3][kNumReasons] = {};
  size_t first_builds[3][kNumReasons] = {};
  double build_rss_kb[3][kNumReasons] = {};
  slang::flat_hash_set<const slang::ast::InstanceBodySymbol *> built;
  size_t escapes[3] = {0, 0, 0};
  size_t escape_first[3] = {0, 0, 0};
  double escape_rss_kb[3] = {0, 0, 0};
  slang::flat_hash_set<const slang::ast::InstanceSymbol *> escaped;
  size_t signals[3] = {0, 0, 0};
  double signal_rss_kb[3] = {0, 0, 0};
  double signal_s[3] = {0, 0, 0};
};

static CanonicalStats g_canon_stats = [] {
  CanonicalStats s;
  const char *v = std::getenv("RTL_TRACE_CANONICAL_STATS");
  s.on = v != nullptr && *v != '\0' && !(v[0] == '0' && v[1] == '\0');
  return s;
}();

// Fast RSS read (pread on a kept-open /proc/self/statm), KiB.
static long CanonStatsRssKB() {
  CanonicalStats &st = g_canon_stats;
  if (st.statm_fd < 0) {
    st.statm_fd = ::open("/proc/self/statm", O_RDONLY);
    st.page_kb = sysconf(_SC_PAGE_SIZE) / 1024;
    if (st.statm_fd < 0) return 0;
  }
  char buf[128];
  const ssize_t n = ::pread(st.statm_fd, buf, sizeof(buf) - 1, 0);
  if (n <= 0) return 0;
  buf[n] = '\0';
  long size = 0, resident = 0;
  if (std::sscanf(buf, "%ld %ld", &size, &resident) != 2) return 0;
  return resident * st.page_kb;
}

static bool CanonBodyHasIfacePort(const slang::ast::InstanceBodySymbol &body) {
  for (const slang::ast::Symbol *p : body.getPortList())
    if (p != nullptr && p->kind == slang::ast::SymbolKind::InterfacePort) return true;
  return false;
}

// True when slang skipped elaborating this instance's body because an identical canonical body exists
// (ElabVisitors.h tryApplyFromCache: interfaces keep a canonical pointer but are still visited).
static bool CanonIsSkippedInstance(const slang::ast::InstanceSymbol &inst) {
  const slang::ast::InstanceBodySymbol *c = inst.getCanonicalBody();
  if (c == nullptr || c == &inst.body) return false;
  return inst.getDefinition().definitionKind != slang::ast::DefinitionKind::Interface;
}

static void CanonStatsInit(const slang::ast::Symbol *any) {
  CanonicalStats &st = g_canon_stats;
  st.init = true;
  const slang::ast::Symbol *s = any;
  while (s != nullptr && s->kind != slang::ast::SymbolKind::Root) {
    const slang::ast::Scope *p = s->getHierarchicalParent();
    s = (p != nullptr) ? &p->asSymbol() : nullptr;
  }
  if (s == nullptr) return;
  const auto &root = s->as<slang::ast::RootSymbol>();
  std::function<void(const slang::ast::Scope &, uint8_t)> visit_scope;
  std::function<void(const slang::ast::Symbol &, uint8_t)> visit_inst = [&](const slang::ast::Symbol &m,
                                                                            uint8_t parent_cls) {
    if (m.kind == slang::ast::SymbolKind::InstanceArray) {
      for (const slang::ast::Symbol *e : m.as<slang::ast::InstanceArraySymbol>().elements)
        if (e != nullptr) visit_inst(*e, parent_cls);
      return;
    }
    if (m.kind != slang::ast::SymbolKind::Instance) return;
    const auto &inst = m.as<slang::ast::InstanceSymbol>();
    const bool skipped = CanonIsSkippedInstance(inst);
    const uint8_t cls = parent_cls != 0 ? 2 : (skipped ? 1 : 0);
    st.body_class[&inst.body] = cls;
    ++st.n_inst[cls];
    if (skipped) {
      st.canonical_bodies.insert(inst.getCanonicalBody());
      if (cls == 1 && CanonBodyHasIfacePort(*inst.getCanonicalBody())) ++st.n_iface_skip_roots;
    }
    visit_scope(inst.body, cls);
  };
  visit_scope = [&](const slang::ast::Scope &scope, uint8_t cls) {
    for (const slang::ast::Symbol &m : scope.members()) {
      switch (m.kind) {
      case slang::ast::SymbolKind::Instance:
      case slang::ast::SymbolKind::InstanceArray:
        visit_inst(m, cls);
        break;
      case slang::ast::SymbolKind::GenerateBlock:
        if (!m.as<slang::ast::GenerateBlockSymbol>().isUninstantiated) visit_scope(m.as<slang::ast::GenerateBlockSymbol>(), cls);
        break;
      case slang::ast::SymbolKind::GenerateBlockArray:
        for (const slang::ast::Symbol *e : m.as<slang::ast::GenerateBlockArraySymbol>().entries) {
          const auto &g = e->as<slang::ast::GenerateBlockSymbol>();
          if (!g.isUninstantiated) visit_scope(g, cls);
        }
        break;
      default:
        break;
      }
    }
  };
  for (const slang::ast::InstanceSymbol *top : root.topInstances) visit_inst(*top, 0);
}

static uint8_t CanonStatsClass(const slang::ast::InstanceBodySymbol *body) {
  auto it = g_canon_stats.body_class.find(body);
  return it == g_canon_stats.body_class.end() ? 0 : it->second;
}

static void CanonStatsOnBuild(const slang::ast::InstanceBodySymbol &body, long rss_delta_kb) {
  CanonicalStats &st = g_canon_stats;
  const uint8_t cls = CanonStatsClass(&body);
  const int r = st.reason;
  ++st.builds[cls][r];
  if (st.built.insert(&body).second) ++st.first_builds[cls][r];
  st.build_rss_kb[cls][r] += static_cast<double>(rss_delta_kb);
}

static void CanonStatsOnEscape(const slang::ast::InstanceSymbol &inst, long rss_delta_kb) {
  CanonicalStats &st = g_canon_stats;
  // The escape binds inst's port connections, which live in inst's parent body.
  const slang::ast::Scope *ps = inst.getParentScope();
  const slang::ast::InstanceBodySymbol *parent = nullptr;
  for (const slang::ast::Scope *sc = ps; sc != nullptr;) {
    const slang::ast::Symbol &sym = sc->asSymbol();
    if (sym.kind == slang::ast::SymbolKind::InstanceBody) {
      parent = &sym.as<slang::ast::InstanceBodySymbol>();
      break;
    }
    if (sym.kind == slang::ast::SymbolKind::Root) break;
    sc = sym.getHierarchicalParent();
  }
  const uint8_t cls = parent != nullptr ? CanonStatsClass(parent) : 0;
  ++st.escapes[cls];
  if (st.escaped.insert(&inst).second) ++st.escape_first[cls];
  st.escape_rss_kb[cls] += static_cast<double>(rss_delta_kb);
}

static void CanonStatsReport() {
  CanonicalStats &st = g_canon_stats;
  if (!st.on || !st.init) return;
  static const char *kCls[3] = {"bound", "skip-root", "skip-sub"};
  std::cout << "[Canon] instances bound=" << st.n_inst[0] << " skip-root=" << st.n_inst[1]
            << " skip-sub=" << st.n_inst[2] << " distinct_canonical_bodies=" << st.canonical_bodies.size()
            << " iface_port_skip_roots=" << st.n_iface_skip_roots << "\n";
  for (int c = 0; c < 3; ++c) {
    std::cout << "[Canon] signals class=" << kCls[c] << " n=" << st.signals[c]
              << " rss_delta=" << static_cast<long>(st.signal_rss_kb[c] / 1024) << "MB time=" << std::fixed
              << std::setprecision(1) << st.signal_s[c] << "s\n";
    for (int r = 0; r < CanonicalStats::kNumReasons; ++r) {
      std::cout << "[Canon] index_build class=" << kCls[c] << " via=" << (r == 0 ? "own-body" : "port-down")
                << " builds=" << st.builds[c][r] << " first=" << st.first_builds[c][r]
                << " rss_delta=" << static_cast<long>(st.build_rss_kb[c][r] / 1024) << "MB\n";
    }
    std::cout << "[Canon] port_up parent_class=" << kCls[c] << " calls=" << st.escapes[c]
              << " distinct_instances=" << st.escape_first[c]
              << " rss_delta=" << static_cast<long>(st.escape_rss_kb[c] / 1024) << "MB\n";
  }
  std::cout.flush();
}

struct CanonStatsSignalScope {
  const slang::ast::Symbol *sym;
  long rss0 = 0;
  std::chrono::steady_clock::time_point t0;
  explicit CanonStatsSignalScope(const slang::ast::Symbol *s) : sym(s) {
    if (!g_canon_stats.on) return;
    if (!g_canon_stats.init) CanonStatsInit(s);
    g_canon_stats.reason = CanonicalStats::kStart;
    rss0 = CanonStatsRssKB();
    t0 = std::chrono::steady_clock::now();
  }
  ~CanonStatsSignalScope() {
    if (!g_canon_stats.on) return;
    const slang::ast::Symbol *b = sym;
    while (b != nullptr && b->kind != slang::ast::SymbolKind::InstanceBody && b->kind != slang::ast::SymbolKind::Root) {
      const slang::ast::Scope *p = b->getHierarchicalParent();
      b = p != nullptr ? &p->asSymbol() : nullptr;
    }
    const uint8_t cls = (b != nullptr && b->kind == slang::ast::SymbolKind::InstanceBody)
                            ? CanonStatsClass(&b->as<slang::ast::InstanceBodySymbol>())
                            : 0;
    ++g_canon_stats.signals[cls];
    g_canon_stats.signal_rss_kb[cls] += static_cast<double>(CanonStatsRssKB() - rss0);
    g_canon_stats.signal_s[cls] += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  }
};

struct CanonStatsReasonScope {
  int saved;
  explicit CanonStatsReasonScope(int r) : saved(g_canon_stats.reason) { g_canon_stats.reason = r; }
  ~CanonStatsReasonScope() { g_canon_stats.reason = saved; }
};
