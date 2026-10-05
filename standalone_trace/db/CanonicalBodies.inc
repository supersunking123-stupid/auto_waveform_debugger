// CanonicalBodies.inc — included by GraphDb.cc after BuildSignalRecord.
//
// Default (RTL_TRACE_CANONICAL_BODIES=0 disables it): trace through canonical instance bodies instead of binding
// every body.
//
// slang elaborates one canonical body per parameter-identical module instance; every other instance's body
// is skipped by elaboration (InstanceSymbol::getCanonicalBody() != nullptr) and is only bound lazily when the
// build loop touches it. This tracer never indexes a skipped body. A signal of a skipped body is traced on its
// structural image inside a body that elaboration already bound, and every hierarchical path in the resulting
// endpoints is translated back through a chain of frames:
//
//   frame = (crossing instance `image`, canonical body `rep` standing in for image->body),
//   translation: paths under path(rep) are rewritten to path(image), then the outer frame applies.
//
// Traversal is the same as ComputeIndexedTraceResults/CollectPortConnectionResults, except
//   - descending through a port into a skipped child instance K enters K's canonical body in a new frame,
//   - leaving a frame's canonical body upward through a port uses the frame's crossing instance (whose port
//     connections were bound by elaboration of its parent) instead of the canonical body's own instance,
//   - the visited set is keyed by (symbol, frame), which is a bijection with the actual symbols.
// ResolveTraceResult is unchanged; its EndpointRecord is translated (path, lhs/rhs refs, re-sorted by path).
// Anything the mapping cannot handle falls back to the baseline BuildSignalRecord on the actual symbol, which
// is always correct (it just binds the body as before).

SymbolRefList CollectLhsSignals(const slang::ast::AssignmentExpression *assignment);
SymbolRefList CollectRhsSignals(const slang::ast::AssignmentExpression *assignment);

// Connections are cached per crossing instance, not per (instance, outer-frame) pair.
// Identity entries matter: an external interface root still belongs to this mapping.
struct CanonIfaceMap {
  enum Reason { None, Unresolved, Shape, Conflict, AliasSplit, AliasMerge, Forwarding, SelfRef };
  Reason reason = None;
  std::vector<std::pair<std::string, std::string>> roots;
};

struct CanonFrame {
  const CanonFrame *outer = nullptr;
  const slang::ast::InstanceSymbol *image = nullptr;    // crossing instance, member of a body in `outer` coordinates
  const slang::ast::InstanceBodySymbol *rep = nullptr;  // canonical body standing in for image->body
  std::string_view src;                                 // hierarchical path of rep (its canonical instance)
  std::string dst;                                      // hierarchical path of image
  const CanonIfaceMap *ifaces = nullptr;
};

struct CanonPtrPairHash {
  size_t operator()(const std::pair<const void *, const void *> &p) const noexcept {
    const size_t a = reinterpret_cast<size_t>(p.first);
    const size_t b = reinterpret_cast<size_t>(p.second);
    return slang::hash<size_t>()(a * 0x9E3779B97F4A7C15ull ^ (b + 0x632BE59BD9B4E019ull));
  }
};

class CanonicalTracer {
 public:
  using SymbolIdMap = slang::flat_hash_map<const slang::ast::Symbol *, uint32_t>;
  using StringIndex = slang::flat_hash_map<std::string_view, uint32_t>;

  CanonicalTracer(const slang::SourceManager &sm, TraceCompileCache &cache, CompileContext &compile_ctx,
                  const SymbolIdMap &symbol_path_ids, const std::vector<std::string> &strings,
                  const StringIndex &string_index)
      : sm_(sm), cache_(cache), ctx_(compile_ctx), ids_(symbol_path_ids), strings_(strings),
        string_index_(string_index) {
    uint32_t max_id = 0;
    for (const auto &kv : ids_) max_id = std::max(max_id, kv.second);
    sym_by_id_.assign(static_cast<size_t>(max_id) + 1, nullptr);
    for (const auto &kv : ids_) sym_by_id_[kv.second] = kv.first;
    no_iface_exclusion_ = EnvFlagEnabled("RTL_TRACE_CANONICAL_ALLOW_IFACE");
    verify_ = EnvFlagEnabled("RTL_TRACE_CANONICAL_VERIFY");
  }

  // RTL_TRACE_CANONICAL_VERIFY=1 (diagnostic only; binds every body like the baseline): also build the baseline
  // record for each signal and report the first mismatches. The DB is still written from the canonical record.
  SignalRecord Build(const slang::ast::Symbol *sym) {
    SignalRecord rec = BuildCanonical(sym);
    if (verify_) Verify(sym, rec);
    return rec;
  }

 private:
  static bool SameEndpoint(const EndpointRecord &a, const EndpointRecord &b) {
    return SameEndpointFields(a, b);
  }
  std::string DescribeEndpoint(const EndpointRecord &e) const {
    auto ids = [&](const std::vector<uint32_t> &v) {
      std::string s;
      for (uint32_t id : v) s += (id < strings_.size() ? strings_[id] : std::string("?")) + ",";
      return s;
    };
    auto strs = [](const std::vector<std::string> &v) {
      std::string s;
      for (const std::string &x : v) s += x + ",";
      return s;
    };
    return "path=" + (e.path_id < strings_.size() ? "#" + strings_[e.path_id] : e.path) + " line=" +
           std::to_string(e.line) + " bits=" + e.bit_map + " lhs_ids=[" + ids(e.lhs_signal_ids) + "] lhs=[" +
           strs(e.lhs_signals) + "] rhs_ids=[" + ids(e.rhs_signal_ids) + "] rhs=[" + strs(e.rhs_signals) + "]";
  }
  void Verify(const slang::ast::Symbol *sym, const SignalRecord &rec) {
    ++stats_.verify_signals;
    SignalRecord base = BuildSignalRecord(sym, sm_, cache_, ctx_, &ids_);
    for (int d = 0; d < 2; ++d) {
      const auto &a = d == 0 ? rec.drivers : rec.loads;
      const auto &b = d == 0 ? base.drivers : base.loads;
      bool same = a.size() == b.size();
      size_t first = 0;
      for (; same && first < a.size(); ++first) {
        if (!SameEndpoint(a[first], b[first])) same = false;
      }
      if (same) continue;
      ++stats_.verify_mismatch;
      if (stats_.verify_mismatch > 20) continue;
      if (first > 0) --first;
      std::cout << "[Canon] VERIFY mismatch " << (d == 0 ? "drivers" : "loads") << " signal="
                << sym->getHierarchicalPath() << " canon_n=" << a.size() << " base_n=" << b.size()
                << " at=" << first << "\n";
      if (first < a.size()) std::cout << "[Canon]   canon: " << DescribeEndpoint(a[first]) << "\n";
      if (first < b.size()) std::cout << "[Canon]   base:  " << DescribeEndpoint(b[first]) << "\n";
    }
  }

 public:
  SignalRecord BuildCanonical(const slang::ast::Symbol *sym) {
    const slang::ast::InstanceBodySymbol *body = GetContainingInstance(sym);
    if (body == nullptr) return Baseline(sym, stats_.sig_no_body);
    const InstInfo &info = GetInfo(*body);
    if (info.broken) return Baseline(sym, stats_.sig_broken);
    // Identity frame (no skipped instance above): trace the actual symbol, but still through Compute so that
    // port follows into skipped children are redirected to their canonical bodies.
    const slang::ast::Symbol *rep_sym = sym;
    if (info.frame == nullptr) {
      ++stats_.sig_identity;
    } else {
      rep_sym = MapIntoRep(*sym, info);
      if (rep_sym == nullptr) return Baseline(sym, stats_.sig_map_fail);
      ++stats_.sig_redirected;
    }

    CanonStatsSignalScope canon_stats_scope(sym);
    SignalRecord rec;
    {
      VisitedSet visited;
      visited.insert({rep_sym, info.frame});
      std::vector<Res> res;
      Compute</*DRIVERS*/ true>(rep_sym, info.frame, visited, res);
      rec.drivers.reserve(res.size());
      for (const Res &r : res) {
        rec.drivers.push_back(ResolveTraceResult(r.result, sm_, true, &cache_, ctx_, &ids_));
        Translate(rec.drivers.back(), r.result, r.frame);
        if (std::any_of(rec.drivers.back().compact_port_routes.begin(),rec.drivers.back().compact_port_routes.end(),
              [](const auto &route){return route.target_path_id==std::numeric_limits<uint32_t>::max();}))
          return Baseline(sym,stats_.sig_map_fail);
      }
    }
    {
      VisitedSet visited;
      visited.insert({rep_sym, info.frame});
      std::vector<Res> res;
      Compute</*DRIVERS*/ false>(rep_sym, info.frame, visited, res);
      rec.loads.reserve(res.size());
      for (const Res &r : res) {
        rec.loads.push_back(ResolveTraceResult(r.result, sm_, false, &cache_, ctx_, &ids_));
        Translate(rec.loads.back(), r.result, r.frame);
        if (std::any_of(rec.loads.back().compact_port_routes.begin(),rec.loads.back().compact_port_routes.end(),
              [](const auto &route){return route.target_path_id==std::numeric_limits<uint32_t>::max();}))
          return Baseline(sym,stats_.sig_map_fail);
      }
    }
    return rec;
  }

  void Report() const {
    std::cout << "[Canon] tracer signals identity=" << stats_.sig_identity << " redirected=" << stats_.sig_redirected
              << " broken=" << stats_.sig_broken << " map_fail=" << stats_.sig_map_fail
              << " no_body=" << stats_.sig_no_body << " bodies_info=" << infos_.size() << " frames=" << frames_.size()
              << " rep_child_maps=" << child_maps_.size() << " excluded_crossings(iface)=" << stats_.excl_iface
              << " excluded_crossings(kind)=" << stats_.excl_kind << " child_lookup_fail=" << stats_.child_lookup_fail
              << " port_map_fail=" << stats_.port_map_fail << " down_map_fail=" << stats_.down_map_fail
              << " xlate_anomaly=" << stats_.xlate_anomaly << " xlate_id_miss=" << stats_.xlate_id_miss
              << " iface_unresolved=" << stats_.iface_unresolved << " iface_shape=" << stats_.iface_shape
              << " iface_conflict=" << stats_.iface_conflict << " iface_alias_split=" << stats_.iface_alias_split
              << " iface_alias_merge=" << stats_.iface_alias_merge << " iface_forwarding=" << stats_.iface_forwarding
              << " iface_selfref=" << stats_.iface_selfref
              << " escape_dead=" << stats_.escape_dead << " virtual_absolute=" << stats_.virtual_absolute << "\n";
    if (verify_) {
      std::cout << "[Canon] verify signals=" << stats_.verify_signals
                << " mismatched_lists=" << stats_.verify_mismatch << "\n";
    }
    std::cout.flush();
  }

 private:
  struct InstInfo {
    const CanonFrame *frame = nullptr;                    // frame in which `rep` is traced (nullptr = identity)
    const slang::ast::InstanceBodySymbol *rep = nullptr;  // body used in place of the actual body
    const std::string *rep_path = nullptr;
    uint32_t actual_len = 0;                              // length of the actual instance path
    bool broken = false;
  };
  struct Res {
    TraceResult result;
    const CanonFrame *frame;
  };
  using VisitedKey = std::pair<const void *, const void *>;
  using VisitedSet = slang::flat_hash_set<VisitedKey, CanonPtrPairHash>;
  using ChildMap = slang::flat_hash_map<std::string, const slang::ast::InstanceSymbol *>;

  struct Stats {
    size_t sig_identity = 0, sig_redirected = 0, sig_broken = 0, sig_map_fail = 0, sig_no_body = 0;
    size_t excl_iface = 0, excl_kind = 0, child_lookup_fail = 0, port_map_fail = 0, down_map_fail = 0;
    size_t xlate_anomaly = 0, xlate_id_miss = 0, escape_dead = 0, virtual_absolute = 0;
    size_t iface_unresolved = 0, iface_shape = 0, iface_conflict = 0;
    size_t iface_alias_split = 0, iface_alias_merge = 0, iface_forwarding = 0, iface_selfref = 0;
    size_t verify_signals = 0, verify_mismatch = 0;
  };

  SignalRecord Baseline(const slang::ast::Symbol *sym, size_t &counter) {
    ++counter;
    return BuildSignalRecord(sym, sm_, cache_, ctx_, &ids_);
  }

  const std::string &RepPath(const slang::ast::InstanceBodySymbol &body) {
    auto it = rep_paths_.find(&body);
    if (it != rep_paths_.end()) return *it->second;
    std::string p = body.parentInstance != nullptr ? body.parentInstance->getHierarchicalPath() : std::string();
    return *rep_paths_.emplace(&body, std::make_unique<std::string>(std::move(p))).first->second;
  }

  bool HasIfacePorts(const slang::ast::InstanceBodySymbol &body) {
    auto it = iface_bodies_.find(&body);
    if (it == iface_bodies_.end()) it = iface_bodies_.emplace(&body, CanonBodyHasIfacePort(body)).first;
    return it->second;
  }

  CanonIfaceMap::Reason PairIfaceRoots(const slang::ast::Symbol *rep, const slang::ast::Symbol *image,
                                      CanonIfaceMap &map) {
    using Reason = CanonIfaceMap;
    if (rep == nullptr || image == nullptr) return Reason::Unresolved;
    if (rep->kind != image->kind) return Reason::Shape;
    if (rep->kind == slang::ast::SymbolKind::InstanceArray) {
      const auto &a = rep->as<slang::ast::InstanceArraySymbol>();
      const auto &b = image->as<slang::ast::InstanceArraySymbol>();
      // Slang rewireIfaceArrayIndices has already put cached connections into formal-port
      // order, including reversed bounds. Pair those leaves, not their printed indices.
      if (a.range != b.range || a.elements.size() != b.elements.size()) return Reason::Shape;
      if (a.elements.empty()) return Reason::Unresolved;
      for (size_t i = 0; i < a.elements.size(); ++i) {
        const auto reason = PairIfaceRoots(a.elements[i], b.elements[i], map);
        if (reason != Reason::None) return reason;
      }
      return Reason::None;
    }
    if (rep->kind != slang::ast::SymbolKind::Instance) return Reason::Unresolved;
    const auto &a = rep->as<slang::ast::InstanceSymbol>();
    const auto &b = image->as<slang::ast::InstanceSymbol>();
    if (!a.isInterface() || !b.isInterface() || &a.getDefinition() != &b.getDefinition()) return Reason::Shape;
    // These connections can refer to roots outside the connected interface itself. Root-only
    // substitution does not model that second connection graph. Module pass-through is fine.
    if (HasIfacePorts(a.body) || HasIfacePorts(b.body)) return Reason::Forwarding;
    std::string src = a.getHierarchicalPath();
    std::string dst = b.getHierarchicalPath();
    for (const auto &root : map.roots) {
      if (root.first == src) return root.second == dst ? Reason::None : Reason::AliasSplit;
      if (root.second == dst) return Reason::AliasMerge;
      // Overlapping roots must describe the same subtree relationship in both directions.
      // Otherwise the longest match would conceal a change in the interface alias graph.
      if ((UnderCanonicalRoot(src, root.first) && dst != root.second + src.substr(root.first.size())) ||
          (UnderCanonicalRoot(root.first, src) && root.second != dst + root.first.substr(src.size())) ||
          (UnderCanonicalRoot(dst, root.second) && src != root.first + dst.substr(root.second.size())) ||
          (UnderCanonicalRoot(root.second, dst) && root.first != src + root.second.substr(dst.size())))
        return Reason::Conflict;
    }
    map.roots.emplace_back(std::move(src), std::move(dst));
    return Reason::None;
  }

  const CanonIfaceMap &IfaceMap(const slang::ast::InstanceSymbol &image) {
    auto it = iface_maps_.find(&image);
    if (it != iface_maps_.end()) return *it->second;
    auto map = std::make_unique<CanonIfaceMap>();
    const auto &rep = *image.getCanonicalBody();
    const auto rp = rep.getPortList();
    const auto ip = image.body.getPortList();
    if (rep.parentInstance == nullptr || rp.size() != ip.size()) {
      map->reason = CanonIfaceMap::Shape;
    } else {
      // InstanceCacheKey has already resolved these connections before accepting a crossing.
      // getIfaceConn only reads cached symbols; never inspect a skipped body's expressions.
      const auto rc = rep.parentInstance->getPortConnections();
      const auto ic = image.getPortConnections();
      auto connection = [](auto connections, const slang::ast::Symbol *port) {
        for (const auto *conn : connections)
          if (&conn->port == port) return conn->getIfaceConn();
        return slang::ast::PortConnection::IfaceConn{nullptr, nullptr};
      };
      for (size_t i = 0; i < rp.size(); ++i) {
        if (rp[i] == nullptr || ip[i] == nullptr || rp[i]->kind != ip[i]->kind || rp[i]->name != ip[i]->name) {
          map->reason = CanonIfaceMap::Shape;
          break;
        }
        if (rp[i]->kind != slang::ast::SymbolKind::InterfacePort) continue;
        const auto a = connection(rc, rp[i]);
        const auto b = connection(ic, ip[i]);
        if (bool(a.second) != bool(b.second) || (a.second != nullptr && a.second->name != b.second->name)) {
          map->reason = CanonIfaceMap::Shape;
          break;
        }
        map->reason = PairIfaceRoots(a.first, b.first, *map);
        if (map->reason != CanonIfaceMap::None) break;
      }
    }
    if (map->reason == CanonIfaceMap::None) {
      const std::string &rep_path = RepPath(rep);
      const std::string image_path = image.getHierarchicalPath();
      for (const auto &root : map->roots) {
        // A port can point down into its own module's local interface. Its source
        // root then also matches local-body signals, so it must agree with the
        // module rewrite. Keep consistent roots for alias checks and translation.
        // An external source targeting an image-local interface is still valid.
        if (UnderCanonicalRoot(root.first, rep_path) &&
            root.second != image_path + root.first.substr(rep_path.size())) {
          map->reason = CanonIfaceMap::SelfRef;
          break;
        }
      }
    }
    if (map->reason != CanonIfaceMap::None) map->roots.clear();
    // Longest boundary match wins. Applying only one entry avoids same-frame remapping chains.
    std::sort(map->roots.begin(), map->roots.end(), [](const auto &a, const auto &b) {
      return a.first.size() > b.first.size();
    });
    return *iface_maps_.emplace(&image, std::move(map)).first->second;
  }

  bool IsCrossing(const slang::ast::InstanceSymbol &inst) {
    const slang::ast::InstanceBodySymbol *c = inst.getCanonicalBody();
    if (c == nullptr || c == &inst.body) return false;
    if (inst.getDefinition().definitionKind != slang::ast::DefinitionKind::Module) {
      ++stats_.excl_kind;
      return false;
    }
    if (HasIfacePorts(*c)) {
      const auto reason = IfaceMap(inst).reason;
      // This diagnostic bypass intentionally disables correctness protection for unsupported
      // mappings. Supported maps are still used. Never retain a partially constructed map.
      if (reason != CanonIfaceMap::None && !no_iface_exclusion_) {
        ++stats_.excl_iface;
        switch (reason) {
        case CanonIfaceMap::Unresolved: ++stats_.iface_unresolved; break;
        case CanonIfaceMap::Shape: ++stats_.iface_shape; break;
        case CanonIfaceMap::Conflict: ++stats_.iface_conflict; break;
        case CanonIfaceMap::AliasSplit: ++stats_.iface_alias_split; break;
        case CanonIfaceMap::AliasMerge: ++stats_.iface_alias_merge; break;
        case CanonIfaceMap::Forwarding: ++stats_.iface_forwarding; break;
        case CanonIfaceMap::SelfRef: ++stats_.iface_selfref; break;
        case CanonIfaceMap::None: break;
        }
        return false;
      }
    }
    return true;
  }

  const CanonFrame *InternFrame(const CanonFrame *outer, const slang::ast::InstanceSymbol &image) {
    const VisitedKey key{outer, &image};
    auto it = frames_.find(key);
    if (it != frames_.end()) return it->second.get();
    auto f = std::make_unique<CanonFrame>();
    f->outer = outer;
    f->image = &image;
    f->rep = image.getCanonicalBody();
    f->src = RepPath(*f->rep);
    f->dst = image.getHierarchicalPath();
    if (HasIfacePorts(*f->rep)) f->ifaces = &IfaceMap(image);
    return frames_.emplace(key, std::move(f)).first->second.get();
  }

  void CollectChildInstances(const slang::ast::Symbol &m, size_t prefix_len, ChildMap &out) {
    switch (m.kind) {
    case slang::ast::SymbolKind::Instance: {
      std::string p = m.getHierarchicalPath();
      if (p.size() >= prefix_len) out.emplace(p.substr(prefix_len), &m.as<slang::ast::InstanceSymbol>());
      break;
    }
    case slang::ast::SymbolKind::InstanceArray:
      for (const slang::ast::Symbol *e : m.as<slang::ast::InstanceArraySymbol>().elements)
        if (e != nullptr) CollectChildInstances(*e, prefix_len, out);
      break;
    case slang::ast::SymbolKind::GenerateBlock:
      if (!m.as<slang::ast::GenerateBlockSymbol>().isUninstantiated)
        for (const slang::ast::Symbol &c : m.as<slang::ast::GenerateBlockSymbol>().members())
          CollectChildInstances(c, prefix_len, out);
      break;
    case slang::ast::SymbolKind::GenerateBlockArray:
      for (const slang::ast::Symbol *e : m.as<slang::ast::GenerateBlockArraySymbol>().entries)
        if (e != nullptr) CollectChildInstances(*e, prefix_len, out);
      break;
    default:
      break;
    }
  }

  const slang::ast::InstanceSymbol *LookupChild(const slang::ast::InstanceBodySymbol &rep, std::string_view rel) {
    auto it = child_maps_.find(&rep);
    if (it == child_maps_.end()) {
      auto m = std::make_unique<ChildMap>();
      const size_t prefix_len = RepPath(rep).size();
      for (const slang::ast::Symbol &c : rep.members()) CollectChildInstances(c, prefix_len, *m);
      it = child_maps_.emplace(&rep, std::move(m)).first;
    }
    auto cit = it->second->find(std::string(rel));
    return cit == it->second->end() ? nullptr : cit->second;
  }

  // Frame/representative body for an actual instance body (memoized, computed from the parent's).
  const InstInfo &GetInfo(const slang::ast::InstanceBodySymbol &body) {
    auto it = infos_.find(&body);
    if (it != infos_.end()) return it->second;
    InstInfo info;
    const slang::ast::InstanceSymbol *inst = body.parentInstance;
    const slang::ast::InstanceBodySymbol *parent = nullptr;
    if (inst != nullptr && inst->getParentScope() != nullptr)
      parent = GetContainingInstance(&inst->getParentScope()->asSymbol());
    const std::string inst_path = inst != nullptr ? inst->getHierarchicalPath() : std::string();
    info.actual_len = static_cast<uint32_t>(inst_path.size());
    if (inst == nullptr || parent == nullptr) {
      info.rep = &body;
    } else {
      const InstInfo pinfo = GetInfo(*parent);  // copy: infos_ may rehash below
      const slang::ast::InstanceSymbol *image = nullptr;
      if (pinfo.broken) {
        info.broken = true;
      } else if (pinfo.frame == nullptr) {
        image = inst;
      } else if (inst_path.size() >= pinfo.actual_len) {
        image = LookupChild(*pinfo.rep, std::string_view(inst_path).substr(pinfo.actual_len));
      }
      if (!info.broken && image == nullptr) {
        ++stats_.child_lookup_fail;
        info.broken = true;
      }
      if (!info.broken) {
        if (IsCrossing(*image)) {
          info.frame = InternFrame(pinfo.frame, *image);
          info.rep = image->getCanonicalBody();
        } else {
          info.frame = pinfo.frame;
          info.rep = &image->body;
        }
      }
    }
    if (!info.broken && info.rep != nullptr) info.rep_path = &RepPath(*info.rep);
    return infos_.emplace(&body, info).first->second;
  }

  std::string SymbolPath(const slang::ast::Symbol &sym) const {
    auto it = ids_.find(&sym);
    if (it != ids_.end()) return strings_[it->second];
    return sym.getHierarchicalPath();
  }

  const slang::ast::Symbol *LookupSignalByPath(const std::string &path) const {
    auto it = string_index_.find(std::string_view(path));
    if (it == string_index_.end() || it->second >= sym_by_id_.size()) return nullptr;
    return sym_by_id_[it->second];
  }

  const slang::ast::Symbol *MapIntoRep(const slang::ast::Symbol &sym, const InstInfo &info) const {
    const std::string p = SymbolPath(sym);
    if (p.size() < info.actual_len || info.rep_path == nullptr) return nullptr;
    const slang::ast::Symbol *r = LookupSignalByPath(*info.rep_path + p.substr(info.actual_len));
    if (r == nullptr || r->kind != sym.kind) return nullptr;
    return r;
  }

  // Port-internal symbol of skipped child `child` -> the same symbol in `rep` (child's canonical body).
  const slang::ast::Symbol *MapDown(const slang::ast::InstanceSymbol &child, const slang::ast::Symbol &internal,
                                    const slang::ast::InstanceBodySymbol &rep) {
    if (internal.getParentScope() == static_cast<const slang::ast::Scope *>(&child.body) && !internal.name.empty()) {
      const slang::ast::Symbol *r = rep.find(internal.name);
      if (r != nullptr && r->kind == internal.kind) return r;
    }
    const std::string p = SymbolPath(internal);
    const std::string cp = child.getHierarchicalPath();
    if (p.size() < cp.size() || p.compare(0, cp.size(), cp) != 0) return nullptr;
    const slang::ast::Symbol *r = LookupSignalByPath(RepPath(rep) + p.substr(cp.size()));
    if (r == nullptr || r->kind != internal.kind) return nullptr;
    return r;
  }

  class EscapeCollector
      : public slang::ast::ASTVisitor<EscapeCollector, slang::ast::VisitFlags::AllGood> {
   public:
    EscapeCollector(std::vector<Res> &out, VisitedSet &visited, const CanonFrame *frame,
                    const slang::ast::InstanceSymbol *instance, const slang::ast::PortSymbol *port)
        : out_(out), visited_(visited), frame_(frame), instance_(instance), port_(port) {}

    void handle(const slang::ast::NamedValueExpression &nve) {
      if (!IsTraceable(&nve.symbol)) return;
      if (!visited_.insert({&nve.symbol, frame_}).second) return;
      Push(nve, &nve.symbol, {}, 0, 0);
    }

    void handle(const slang::ast::MemberAccessExpression &expr) {
      auto mai = ResolveStructMemberAccess(expr);
      if (mai.resolved) {
        if (!visited_member_keys_.insert({mai.parent_symbol, mai.member_path}).second) return;
        Push(expr, mai.parent_symbol, std::move(mai.member_path), mai.bit_offset, mai.bit_width);
        return;
      }
      const slang::ast::Symbol *sym = expr.getSymbolReference();
      if (!IsTraceable(sym)) return;
      if (!visited_.insert({sym, frame_}).second) return;
      Push(expr, sym, {}, 0, 0);
    }

   private:
    void Push(const slang::ast::Expression &e, const slang::ast::Symbol *sym, std::string member_path,
              uint64_t off, uint64_t width) {
      ExprTraceResult result;
      result.expr = &e;
      result.symbol = sym;
      result.assignment = nullptr;
      result.context_from_instance_port = true;
      result.context_instance = instance_;
      result.context_port = port_;
      result.member_path = std::move(member_path);
      result.member_bit_offset = off;
      result.member_bit_width = width;
      out_.push_back(Res{std::move(result), frame_});
    }

    std::vector<Res> &out_;
    VisitedSet &visited_;
    const CanonFrame *frame_;
    const slang::ast::InstanceSymbol *instance_;
    const slang::ast::PortSymbol *port_;
    std::set<std::pair<const slang::ast::Symbol *, std::string>> visited_member_keys_;
  };

  // Follow the actual instantiating connection, leaving the current canonical
  // frame when it represents this body. No reverse endpoint references are needed.
  void Escape(const slang::ast::PortSymbol &port, const slang::ast::InstanceBodySymbol &x,
              const CanonFrame *f, VisitedSet &visited,
              std::vector<Res> &out) {
    const slang::ast::InstanceSymbol *inst = x.parentInstance;
    const CanonFrame *ef = f;
    if (f != nullptr && f->rep == &x) {
      inst = f->image;
      ef = f->outer;
    }
    if (inst == nullptr) return;
    const slang::ast::Symbol *target = &port;
    if (&inst->body != &x) {
      // Same port position in the crossing instance's own (skipped) body.
      target = PortImagePort(port, x, inst->body);
      if (target == nullptr || target->kind != port.kind) {
        ++stats_.port_map_fail;
        return;
      }
    }
    const long canon_rss0 = g_canon_stats.on ? CanonStatsRssKB() : 0;
    for (const slang::ast::PortConnection *conn : inst->getPortConnections()) {
      const auto *conn_port = conn->port.template as_if<slang::ast::PortSymbol>();
      if (conn_port == nullptr || conn_port != target) continue;
      const slang::ast::Expression *expr = conn->getExpression();
      if (expr == nullptr) continue;
      EscapeCollector collector(out, visited, ef, inst, conn_port);
      expr->visit(collector);
      break;
    }
    if (g_canon_stats.on) CanonStatsOnEscape(*inst, CanonStatsRssKB() - canon_rss0);
  }

  struct MappingEscape {
    const slang::ast::InstanceSymbol *instance = nullptr;
    const slang::ast::Expression *expr = nullptr;
    const CanonFrame *frame = nullptr;
    const slang::ast::PortSymbol *port = nullptr;
  };
  MappingEscape EscapeMapping(const slang::ast::PortSymbol &port,
                              const slang::ast::InstanceBodySymbol &body, const CanonFrame *frame) {
    const auto *instance = body.parentInstance;
    const auto *outer = frame;
    if (frame && frame->rep == &body) { instance = frame->image; outer = frame->outer; }
    if (!instance) return {};
    const slang::ast::PortSymbol *target = &port;
    if (&instance->body != &body) {
      target = PortImagePort(port, body, instance->body);
      if (!target) { ++stats_.port_map_fail; return {}; }
    }
    return {instance, PortActualConnection(*instance, *target), outer, target};
  }
  template <bool DRIVERS>
  bool ProvedNativeTargets(const std::vector<PortMapPiece> &map) {
    auto representative_map = map;
    for (auto &piece : representative_map) {
      const auto *body = piece.source ? GetContainingInstance(piece.source) : nullptr;
      if (!body) return false;
      const auto *instance = body->parentInstance;
      const auto *rep = instance ? instance->getCanonicalBody() : nullptr;
      if (rep && rep != body) {
        const auto *mapped = MapDown(*instance, *piece.source, *rep);
        if (!mapped) return false;
        piece.source = mapped;
      }
    }
    return PortTargetsHaveProvedNativeAccesses<DRIVERS>(representative_map, cache_);
  }
  template <bool DRIVERS>
  void LegacyCompute(const slang::ast::Symbol *sym, const CanonFrame *f, VisitedSet &visited,
                     std::vector<Res> &out, size_t depth = 0) {
    const auto *body = GetContainingInstance(sym);
    if (!body || depth >= kPortMapMaxDepth) return;
    const auto retained = GetOrBuildBodyTraceIndex(*body, cache_);
    const auto &entries = DRIVERS ? retained->drivers : retained->loads;
    const auto it = entries.find(sym);
    if (it == entries.end()) return;
    for (const auto &entry : it->second) {
      if (const auto *port = std::get_if<const slang::ast::PortSymbol *>(&entry)) {
        const size_t before = out.size();
        if ((*port)->direction == (DRIVERS ? slang::ast::ArgumentDirection::In : slang::ast::ArgumentDirection::Out))
          Escape(**port, *body, f, visited, out);
        if (out.size() == before) out.push_back(Res{*port, f});
      } else if (const auto *expr = std::get_if<ExprTraceResult>(&entry)) {
        bool followed = false;
        if (expr->context_from_instance_port && expr->context_port && expr->context_instance && expr->member_path.empty()) {
          const auto *child = expr->context_instance;
          for (const auto &piece : PortInternalMap(*expr->context_port, cache_)) {
            const auto *csym = piece.source; const auto *cf = f;
            if (!csym) continue;
            if (IsCrossing(*child)) if (const auto *mapped = MapDown(*child, *csym, *child->getCanonicalBody())) {
              csym = mapped; cf = InternFrame(f, *child);
            }
            if (!visited.insert({csym, cf}).second) continue;
            const size_t before = out.size();
            LegacyCompute<DRIVERS>(csym, cf, visited, out, depth + 1);
            followed |= out.size() > before;
          }
        }
        if (!followed) out.push_back(Res{*expr, f});
      }
    }
  }
  template <bool DRIVERS>
  void Compute(const slang::ast::Symbol *sym, const CanonFrame *f, VisitedSet &visited,
               std::vector<Res> &out, bool following_parent = false,
               const PortProjection *projection = nullptr, size_t map_depth = 0) {
    const auto *body = GetContainingInstance(sym);
    if (!body) return;
    const auto retained_index = GetOrBuildBodyTraceIndex(*body, cache_);
    const auto &index = *retained_index;
    const auto &entries = DRIVERS ? index.drivers : index.loads;
    const auto it = entries.find(sym);
    if (it == entries.end()) return;
    auto marker = [&](const PortProjection &p, const slang::ast::PortSymbol *port, bool constant, bool unresolved) {
      out.push_back(Res{PortMappingMarker(p, port, constant, unresolved), constant || unresolved ? p.owner_frame : f});
    };
    std::unordered_set<std::string> fallback_routes;
    auto fallback = [&](const PortProjection &p, const slang::ast::PortSymbol *port) {
      marker(p, port, false, true);
      if (!fallback_routes.insert(PortFallbackKey(p)).second) return;
      VisitedSet legacy_visited; legacy_visited.insert({sym, f});
      if (auto boundary = PortApproximateBoundary(p)) out.push_back(Res{std::move(*boundary), p.owner_frame});
      std::vector<Res> legacy;
      LegacyCompute<DRIVERS>(sym, f, legacy_visited, legacy);
      for (auto &result : legacy) if (PortKeepLegacyResult(p, result.result, cache_)) {
        MarkApproximateTrace(result.result, PortProjectionCoverage(p)); out.push_back(std::move(result));
      }
    };
    if (projection && map_depth >= kPortMapMaxDepth) { fallback(*projection, nullptr); return; }
    for (const auto &entry : it->second) {
      if (const auto *port = std::get_if<const slang::ast::PortSymbol *>(&entry)) {
        if ((*port)->direction == (DRIVERS ? slang::ast::ArgumentDirection::In : slang::ast::ArgumentDirection::Out)) {
          const auto escape = EscapeMapping(**port, *body, f);
          if (escape.expr) {
            auto initial = projection ? *projection : PortIdentityProjection(sym, f);
            initial.connection = escape.expr;
            const auto internal = PortInternalMap(**port, cache_);
            const auto *actual_port = escape.port;
            const auto conversion = PortInspectOutput(*escape.instance, *actual_port);
            const auto map = PortBoundConnectionMap(*escape.instance, *actual_port, *escape.expr, cache_);
            if (initial.pieces.empty() || internal.empty() || map.empty() || conversion.signed_widening || conversion.unsupported ||
                std::any_of(internal.begin(), internal.end(), [](const auto &p) { return p.kind != PortMapPiece::Exact; })) {
              fallback(initial, *port); continue;
            }
            auto formal = PortComposeDown(initial, internal, *port);
            if (formal.pieces.empty()) continue;
            if (!following_parent && PortInternalIsIdentity(**port) &&
                std::all_of(map.begin(), map.end(), [](const auto &p) { return p.kind == PortMapPiece::Exact; }) &&
                HasLocalPortBridge<DRIVERS>(index, sym, cache_) && ProvedNativeTargets<DRIVERS>(map)) {
              if (projection) { marker(initial, *port, false, false); continue; }
              if (auto route = PortCompactRoute(*port, sym, map, escape.frame, &ids_)) {
                route->connection = escape.expr; out.push_back(Res{std::move(*route), f}); continue;
              }
            }
            auto routes = PortComposeUp(formal, map);
            if (routes.size() > kPortMapMaxPieces) { fallback(initial, *port); continue; }
            const bool retain_boundary = DRIVERS && !following_parent && !projection &&
                RetainUnusedInputBoundary(**port, sym, index, cache_) &&
                std::all_of(map.begin(), map.end(), [](const auto &p) { return p.kind == PortMapPiece::Exact; });
            const bool retain_struct = DRIVERS && !following_parent && !projection &&
                RetainStructInputBoundary(**port, sym, *actual_port, *escape.instance, *escape.expr, map, cache_);
            PortCoverage boundary_coverage = retain_struct ? PortProjectionCoverage(formal) : PortCoverage{};
            for (auto &route : routes) {
              const auto &piece = route.pieces.front();
              if (piece.kind != PortMapPiece::Exact) {
                if (piece.kind == PortMapPiece::Constant) marker(route, *port, true, false);
                else fallback(route, *port);
                continue;
              }
              auto branch_visited = visited;
              if (!branch_visited.insert({piece.source, escape.frame}).second) { fallback(route, *port); continue; }
              const size_t before = out.size();
              Compute<DRIVERS>(piece.source, escape.frame, branch_visited, out, true, &route, map_depth + 1);
              if (retain_boundary && out.size() > before &&
                  std::all_of(out.begin() + before, out.end(), [](const Res &r) { return NonconstantInputBoundaryResult(r.result); })) {
                auto selected = PortProjectionCoverage(route);
                boundary_coverage.insert(boundary_coverage.end(), selected.begin(), selected.end());
              }
              if (out.size() == before) marker(route, *port, false, false);
            }
            PortNormalizeCoverage(boundary_coverage);
            if (!boundary_coverage.empty()) out.push_back(Res{MappedPortTraceResult{*port, std::move(boundary_coverage)}, f});
            continue;
          }
        }
        if (projection) marker(*projection, *port, false, false);
        else out.push_back(Res{*port, f});
        continue;
      }
      const auto *expr = std::get_if<ExprTraceResult>(&entry);
      if (!expr) continue;
      std::optional<PortCoverage> coverage;
      if (projection) {
        coverage = PortLocalCoverage(*projection, *expr, cache_);
        if (!coverage) { fallback(*projection, nullptr); continue; }
        if (coverage->empty()) continue;
      }
      bool followed = false;
      if (expr->context_from_instance_port && expr->context_port && expr->context_instance) {
        const auto *child = expr->context_instance;
        const auto *port = expr->context_port;
        const auto *connection = PortActualConnection(*child, *port);
        const auto map = connection ? PortBoundConnectionMap(*child, *port, *connection, cache_) : std::vector<PortMapPiece>{};
        if (!map.empty() && std::all_of(map.begin(), map.end(), [](const auto &p) { return p.kind == PortMapPiece::Constant; })) continue;
        auto initial = projection ? *projection : PortInitialDownProjection(sym, map, f);
        initial.connection = connection;
        const auto internal = PortInternalMap(*port, cache_);
        const auto conversion = PortInspectOutput(*child, *port);
        if (initial.pieces.empty() || internal.empty() || map.empty() || PortHasUnprovedDependency(map, sym) || conversion.signed_widening || conversion.unsupported ||
            std::any_of(internal.begin(), internal.end(), [](const auto &p) { return p.kind != PortMapPiece::Exact; })) {
          fallback(initial, port); followed = true;
        } else {
          auto formal = PortComposeDown(initial, map, port);
          auto routes = PortComposeUp(formal, internal);
          if (routes.size() > kPortMapMaxPieces) { fallback(initial, port); followed = true; }
          else for (auto &route : routes) {
            const auto *csym = route.pieces.front().source; const auto *cf = f;
            if (!csym) continue;
            if (IsCrossing(*child)) {
              if (const auto *mapped = MapDown(*child, *csym, *child->getCanonicalBody())) {
                csym = mapped; cf = InternFrame(f, *child);
                for (auto &piece : route.pieces) piece.source = mapped;
              } else ++stats_.down_map_fail;
            }
            auto branch_visited = visited;
            if (!branch_visited.insert({csym, cf}).second) { fallback(initial, port); followed = true; continue; }
            const size_t before = out.size();
            Compute<DRIVERS>(csym, cf, branch_visited, out, false, &route, map_depth + 1);
            followed |= out.size() > before;
          }
          if (formal.pieces.empty()) followed = true;
        }
      }
      if (!followed) { auto result = *expr; if (coverage) result.port_query_coverage = *coverage; out.push_back(Res{std::move(result), f}); }
    }
  }

  std::string Xlate(std::string_view s, const CanonFrame *f) {
    std::string out(s);
    bool first = true;
    bool external_iface = false;
    for (const CanonFrame *g = f; g != nullptr; g = g->outer) {
      const auto roots = g->ifaces != nullptr
          ? std::span<const std::pair<std::string, std::string>>(g->ifaces->roots)
          : std::span<const std::pair<std::string, std::string>>();
      const bool mapped = ApplyCanonicalFramePath(out, g->src, g->dst, roots, external_iface);
      if (!mapped && !external_iface && (!first || out.find('.') != std::string::npos)) {
        // Interface roots validated by an inner frame can remain outside an outer module.
        // Still try every outer substitution: a local interface may live in that module.
        if (out.find("::") == std::string::npos) ++stats_.xlate_anomaly;
      }
      first = false;
    }
    return out;
  }

  // slang's virtual-interface placeholder instances (InstanceSymbol::createVirtual, InstanceSymbols.cpp:398-418)
  // are parented to the scope that first named them but never added as members, and are deduplicated globally
  // (Compilation::getOrAddVirtualIface). Their paths are therefore absolute (the same for every image of a shared
  // body) and must not be translated.
  bool UnderVirtualPlaceholder(const slang::ast::Symbol &sym) {
    for (const slang::ast::Scope *s = sym.getParentScope(); s != nullptr;) {
      const slang::ast::Symbol &ss = s->asSymbol();
      if (ss.kind != slang::ast::SymbolKind::InstanceBody) {
        s = ss.getParentScope();
        continue;
      }
      const slang::ast::InstanceSymbol *inst = ss.as<slang::ast::InstanceBodySymbol>().parentInstance;
      if (inst == nullptr) return false;
      const slang::ast::Scope *p = inst->getParentScope();
      if (p == nullptr) return false;
      if (p->asSymbol().kind != slang::ast::SymbolKind::InstanceArray && p->find(inst->name) != inst) {
        ++stats_.virtual_absolute;
        return true;
      }
      s = p;
    }
    return false;
  }

  // Paths (as MaterializeSignalRefs / ResolveTraceResult print them) of the string (non-id) refs of `r` that lie
  // under a virtual-interface placeholder; `abs_path` = the endpoint's own symbol does.
  void CollectAbsolutePaths(const TraceResult &r, std::vector<std::string> &abs, bool &abs_path) {
    const auto *item = std::get_if<ExprTraceResult>(&r);
    if (item == nullptr) return;
    auto scan = [&](const SymbolRefList &list) {
      for (const slang::ast::Symbol *sym : list) {
        if (sym == nullptr || ids_.find(sym) != ids_.end()) continue;
        if (UnderVirtualPlaceholder(*sym)) abs.push_back(std::string(sym->getHierarchicalPath()));
      }
    };
    if (item->assignment != nullptr) {
      if (PerBodyTraceCache *bc = FindBodyTraceCache(cache_, item->trace_body)) {
        scan(GetCachedLhsSignals(item->assignment, *bc));
        scan(GetCachedRhsSignals(item->assignment, *bc));
      } else {
        scan(CollectLhsSignals(item->assignment));
        scan(CollectRhsSignals(item->assignment));
      }
    } else {
      scan(item->context_lhs_signals);
    }
    abs_path = item->symbol != nullptr && ids_.find(item->symbol) == ids_.end() &&
               UnderVirtualPlaceholder(*item->symbol);
  }

  void TranslateRefs(std::vector<uint32_t> &ids, std::vector<std::string> &strs, const CanonFrame *f,
                     const std::vector<std::string> &abs) {
    if (ids.empty() && strs.empty()) return;
    for (std::string &s : strs) {
      if (!abs.empty() && std::find(abs.begin(), abs.end(), s) != abs.end()) continue;
      s = Xlate(s, f);
    }
    size_t w = 0;
    for (size_t i = 0; i < ids.size(); ++i) {
      std::string t = Xlate(strings_[ids[i]], f);
      auto it = string_index_.find(std::string_view(t));
      if (it != string_index_.end()) {
        ids[w++] = it->second;
      } else {
        ++stats_.xlate_id_miss;
        strs.push_back(std::move(t));
      }
    }
    ids.resize(w);
    if (ids.size() > 1)
      std::sort(ids.begin(), ids.end(), [&](uint32_t a, uint32_t b) { return strings_[a] < strings_[b]; });
    // Sort only: the baseline keeps duplicate strings (distinct symbols with the same path, e.g. loop variables
    // of unnamed blocks; see SortUniqueSymbolsByPath), so de-duplicating here would change the DB.
    if (strs.size() > 1) std::sort(strs.begin(), strs.end());
  }

  void Translate(EndpointRecord &e, const TraceResult &r, const CanonFrame *f) {
    if (const auto *compact = std::get_if<CompactPortTraceResult>(&r)) {
      for (auto &route : e.compact_port_routes) {
        const std::string_view name = route.target_path_id != std::numeric_limits<uint32_t>::max()
            ? std::string_view(strings_[route.target_path_id]) : std::string_view(route.target_path);
        const auto target = Xlate(name, compact->target_frame);
        const auto it = string_index_.find(target);
        if (it != string_index_.end()) { route.target_path_id=it->second; route.target_path.clear(); }
        else { route.target_path_id=std::numeric_limits<uint32_t>::max(); route.target_path=target; }
      }
    }
    if (f == nullptr) return;
    std::vector<std::string> abs;
    bool abs_path = false;
    CollectAbsolutePaths(r, abs, abs_path);
    if (e.path_id != std::numeric_limits<uint32_t>::max()) {
      std::string t = Xlate(strings_[e.path_id], f);
      auto it = string_index_.find(std::string_view(t));
      if (it != string_index_.end()) {
        e.path_id = it->second;
      } else {
        ++stats_.xlate_id_miss;
        e.path = std::move(t);
        e.path_id = std::numeric_limits<uint32_t>::max();
      }
    } else if (!e.path.empty() && !abs_path) {
      e.path = Xlate(e.path, f);
    }
    TranslateRefs(e.lhs_signal_ids, e.lhs_signals, f, abs);
    TranslateRefs(e.rhs_signal_ids, e.rhs_signals, f, abs);
  }

  const slang::SourceManager &sm_;
  TraceCompileCache &cache_;
  CompileContext &ctx_;
  const SymbolIdMap &ids_;
  const std::vector<std::string> &strings_;
  const StringIndex &string_index_;
  std::vector<const slang::ast::Symbol *> sym_by_id_;
  bool no_iface_exclusion_ = false;
  bool verify_ = false;
  slang::flat_hash_map<const slang::ast::InstanceBodySymbol *, InstInfo> infos_;
  slang::flat_hash_map<VisitedKey, std::unique_ptr<CanonFrame>, CanonPtrPairHash> frames_;
  slang::flat_hash_map<const slang::ast::InstanceBodySymbol *, std::unique_ptr<std::string>> rep_paths_;
  slang::flat_hash_map<const slang::ast::InstanceBodySymbol *, std::unique_ptr<ChildMap>> child_maps_;
  slang::flat_hash_map<const slang::ast::InstanceBodySymbol *, bool> iface_bodies_;
  slang::flat_hash_map<const slang::ast::InstanceSymbol *, std::unique_ptr<CanonIfaceMap>> iface_maps_;
  Stats stats_;
};
