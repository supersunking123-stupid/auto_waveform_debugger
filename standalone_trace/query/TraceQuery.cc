// TraceQuery.cc — Trace query subcommand implementation.
#include "query/TraceQuery.h"
#include "db/EntryPoints.h"
#include "db/GraphDbTypes.h"
#include "db/GraphDbInternals.h"

#include <iostream>
#include <regex>
#include <string>
#include <unordered_set>
#include <vector>

namespace rtl_trace {

bool CompileUserRegex(const char *option, const std::string &pattern, std::regex *out) {
  try {
    std::regex compiled(pattern);
    if (out != nullptr) *out = std::move(compiled);
    return true;
  } catch (const std::regex_error &e) {
    std::cerr << "Invalid regex for " << option << ": '" << pattern << "' (" << e.what() << ")\n";
    return false;
  }
}

std::vector<EndpointRecord> FindFallbackDriverEndpoints(TraceSession &session, uint32_t target_sig_id) {
  std::vector<EndpointRecord> out;
  if (!session.graph.has_value()) return out;

  const GraphDb &graph = *session.graph;
  if (target_sig_id >= graph.ReadSignals().size()) return out;

  const uint32_t path_id = graph.ReadSignals()[target_sig_id].name_str_id;
  const std::string_view target_signal = SessionSignalName(session, target_sig_id);
  const std::vector<uint32_t> source_sig_ids = SessionAssignmentLhsRefs(session, path_id);
  std::unordered_set<std::string> seen;
  for (uint32_t source_sig_id : source_sig_ids) {
    const SignalRecord &record = SessionSignalRecord(session, source_sig_id);
    for (EndpointRecord e : record.loads) {
      std::vector<std::string> lhs_paths = InferAssignmentLhsPaths(session, e);
      if (lhs_paths.empty()) continue;
      if (std::find(lhs_paths.begin(), lhs_paths.end(), target_signal) == lhs_paths.end()) continue;
      e.lhs_signals = std::move(lhs_paths);
      const std::string key = EndpointKey(session.db, e);
      if (!seen.insert(key).second) continue;
      out.push_back(std::move(e));
    }
  }
  return out;
}

std::optional<TraceRunResult> TryRunGlobalNetFastPath(const TraceDb &db, const TraceOptions &opts) {
  // Compact global sinks include registers that depend on a clock or reset.
  // The reverse map is not a driver map. Compaction preserves driver lists,
  // so use those lists to retain the actual assignments, including clock and
  // reset registers whose names happen to match the global-net heuristic.
  if (opts.mode == "drivers") return std::nullopt;
  TraceRunResult result;
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

TraceRunResult RunTraceQuery(TraceSession &session, const TraceOptions &opts) {
  const TraceDb &db = session.db;
  if (std::optional<TraceRunResult> fast = TryRunGlobalNetFastPath(db, opts)) {
    return *fast;
  }
  TraceRunResult result;

  const bool is_drivers_mode = (opts.mode == "drivers");
  const auto root_id = LookupSignalId(session, opts.root_signal);
  const bool root_is_member = root_id && session.graph->ReadSignals()[*root_id].parent_signal_id !=
                                             std::numeric_limits<uint32_t>::max();
  auto root_matches = [&](const EndpointRecord &e) {
    if (!e.port_query_coverage.empty())
      return EndpointMatchesPortOwner(session, e, *root_id, opts.signal_select_axes);
    if (opts.signal_select_axes.size() != 1)
      return EndpointMatchesSignalAxes(e, opts.signal_select_axes);
    auto select = opts.signal_select_axes.front();
    uint32_t parent_id = *root_id;
    if (root_is_member) {
      const auto &member = session.graph->ReadSignals()[*root_id];
      parent_id = member.parent_signal_id;
      select.first = static_cast<int32_t>(int64_t(select.first) + member.member_bit_offset);
      select.second = static_cast<int32_t>(int64_t(select.second) + member.member_bit_offset);
    }
    // Logical axes of root arrays have their own coordinate space. Struct
    // members, including approximate sibling selectors, use parent bits.
    if (!root_is_member && e.bit_map_logical_axes)
      return EndpointMatchesSignalAxes(e, opts.signal_select_axes);
    return EndpointMatchesParentStructBits(session, e, parent_id, select);
  };
  std::vector<EndpointRecord> logic_endpoints;
  std::vector<EndpointRecord> unresolved_ports;
  std::unordered_set<std::string> seen_logic;
  std::unordered_set<std::string> seen_ports;
  using OwnerDomain = std::vector<std::pair<int32_t, int32_t>>;
  using RouteDomain = std::optional<OwnerDomain>;
  const RouteDomain root_domain = root_id && !opts.signal_select_axes.empty() ?
      SessionPortOwnerSelection(session, *root_id, opts.signal_select_axes) : RouteDomain{};
  std::unordered_set<uint32_t> visited_signals;
  std::unordered_set<std::string> visited_routes;
  std::unordered_set<std::string> stop_once;
  bool node_cap_hit = false;
  // Bound newly exposed approximate port boundaries by the explicit failed
  // chunk's compiled source union. Coarse A logic records still survive.
  bool fallback_port_sources_known = false;
  std::unordered_set<std::string> fallback_port_sources, fallback_root_ports;

  auto record_stop = [&](std::string_view sig, const std::string &reason, const std::string &detail,
                         size_t depth) {
    const std::string key = reason + "\t" + std::string(sig) + "\t" + detail;
    if (!stop_once.insert(key).second) return;
    result.stops.push_back(TraceStop{std::string(sig), reason, detail, depth});
  };

  auto endpoint_allowed = [&](const EndpointRecord &e) {
    const std::string &path = EndpointPath(db, e);
    if (!RegexMatch(opts.include_re, path)) return false;
    if (opts.exclude_re.has_value() && std::regex_search(path, *opts.exclude_re)) return false;
    return true;
  };

  auto connection_detail = [&](const EndpointRecord &e, std::string_view reason) {
    std::string detail(reason);
    const auto expression = e.assignment_text.empty() ? FetchSourceSlice(session, e) : e.assignment_text;
    if (!expression.empty()) detail += "; connection " + expression;
    if (e.line > 0) detail += " at " + EndpointFile(db, e) + ":" + std::to_string(e.line);
    return detail;
  };
  std::function<void(uint32_t, size_t, size_t, const RouteDomain &, bool, const std::string &)> walk_route;
  auto walk_signal = [&](uint32_t id, size_t depth, size_t cone, const RouteDomain &domain) {
    walk_route(id, depth, cone, domain, false, {});
  };
  std::function<void(std::string_view, size_t, size_t)> walk_signal_name =
      [&](std::string_view sig_name, size_t depth, size_t cone_depth) {
        std::optional<uint32_t> sig_id = LookupSignalId(session, sig_name);
        if (!sig_id.has_value()) {
          record_stop(std::string(sig_name), "missing_signal", "not-in-db", depth);
          return;
        }
        walk_signal(*sig_id, depth, cone_depth, RouteDomain{});
      };

  walk_route = [&](uint32_t sig_id, size_t depth, size_t cone_depth, const RouteDomain &domain,
                   bool approximate, const std::string &provenance) {
        if (domain && domain->empty()) return;
        const std::string_view sig = SessionSignalName(session, sig_id);
        if (depth > opts.depth_limit) {
          record_stop(sig, "depth_limit", "max-depth-reached", depth);
          return;
        }
        if (node_cap_hit) return;
        if (visited_routes.size() >= opts.max_nodes) {
          node_cap_hit = true;
          record_stop(sig, "node_limit", "max-nodes-reached", depth);
          return;
        }
        std::string route_key = std::to_string(sig_id) + (approximate ? "~" : "=");
        if (domain) for (const auto &[lo,hi] : *domain)
          route_key += ";" + std::to_string(lo) + ":" + std::to_string(hi);
        else route_key += "*";
        if (!visited_routes.insert(route_key).second) {
          record_stop(sig, "cycle", "already-visited", depth);
          return;
        }
        visited_signals.insert(sig_id);
        if (opts.stop_at_re.has_value() && std::regex_search(sig.begin(), sig.end(), *opts.stop_at_re)) {
          record_stop(sig, "stop_at", "matched-stop-at-regex", depth);
          return;
        }
        if (domain) {
          const auto whole=SessionPortOwnerSelection(session,sig_id,{});
          if (whole && *whole==*domain) {
            auto global_opts=opts;global_opts.root_signal=std::string(sig);global_opts.signal_select_axes.clear();
            if (auto fast=TryRunGlobalNetFastPath(db,global_opts)) {
              for (const auto &e : fast->endpoints)
                if (endpoint_allowed(e) && seen_logic.insert(EndpointKey(db,e)).second) logic_endpoints.push_back(e);
              return;
            }
          }
        }
        const SignalRecord &record = SessionSignalRecord(session, sig_id);
        const std::vector<EndpointRecord> &edges =
            is_drivers_mode ? record.drivers : record.loads;
        std::vector<EndpointRecord> fallback_edges;
        const std::vector<EndpointRecord> *active_edges = &edges;
        if (is_drivers_mode && edges.empty() && sig == opts.root_signal) {
          fallback_edges = FindFallbackDriverEndpoints(session, sig_id);
          if (!fallback_edges.empty()) active_edges = &fallback_edges;
        }

        std::string failure_connection = provenance;
        for (const auto &edge : *active_edges) if (edge.port_mapping_unresolved)
          failure_connection = connection_detail(edge, "connection");
        bool failed_proof = std::any_of(active_edges->begin(), active_edges->end(),
            [](const auto &e) { return e.port_mapping_unresolved; });
        if (sig == opts.root_signal && failed_proof) {
          for (const auto &e : *active_edges) {
            if (!e.bit_map_approximate || e.port_query_coverage.empty() || !root_matches(e)) continue;
            const auto &path = EndpointPath(db,e);
            if (e.kind == EndpointKind::kPort) fallback_root_ports.insert(path);
            else { fallback_port_sources.insert(path); fallback_port_sources_known = true; }
          }
        }
        std::vector<EndpointRecord> constrained_edges;
        if (domain && !approximate) {
          for (const auto &original : *active_edges) {
            const auto projected = SessionConstrainPortEndpoint(session, original, sig_id, *domain);
            if (!projected) {
              record_stop(EndpointPath(db, original), "unresolved_connection_mapping",
                          failure_connection.empty() ? connection_detail(original, "target-owner-domain-is-unproven")
                                             : "target-owner-domain-is-unproven; " + failure_connection, depth);
              failed_proof = true;
              auto fallback = original; fallback.bit_map_approximate = true;
              constrained_edges.push_back(std::move(fallback));
              continue;
            }
            constrained_edges.insert(constrained_edges.end(), projected->begin(), projected->end());
          }
          active_edges = &constrained_edges;
        }
        std::vector<EndpointRecord> legacy_hop_edges;
        if (approximate && !domain) {
          legacy_hop_edges = SessionLegacyHopEndpointView(session, *active_edges);
          active_edges = &legacy_hop_edges;
        }
        bool edge_approximate = approximate;
        std::string actual_connection = failure_connection;
        auto descend = [&](uint32_t id, size_t d, size_t cone, const RouteDomain &selected) {
          walk_route(id, d, cone, edge_approximate ? RouteDomain{} : selected,
                     edge_approximate, actual_connection);
        };
        auto follow_bridge = [&](uint32_t source_id, uint32_t target_id, size_t next_cone) {
          if (edge_approximate || !(session.graph->format_features & kDbPortQueryCoverage)) {
            descend(target_id, depth + 1, next_cone, RouteDomain{});
            return true;
          }
          RouteDomain selected_source;
          if (source_id == sig_id && domain) selected_source = domain;
          else if (root_id && source_id == *root_id) selected_source = root_domain;
          const auto target_domain = SessionPortBridgeDomain(session, is_drivers_mode,
                                                             source_id, target_id, selected_source);
          if (!target_domain) {
            // Historical feature1/3 paths without mapped declarations retain
            // their whole-signal traversal. A selected or mapped route refuses
            // an unproved inverse instead of reaching a neighboring array row.
            if (selected_source || SessionPortOwnerSelection(session, target_id, {})) {
              record_stop(SessionSignalName(session, target_id), "unresolved_connection_mapping",
                          "reverse-port-domain-is-unproven", depth);
              edge_approximate = true;
              descend(target_id, depth + 1, next_cone, RouteDomain{});
              return true;
            }
            descend(target_id, depth + 1, next_cone, RouteDomain{});
          } else descend(target_id, depth + 1, next_cone, target_domain);
          return true;
        };
        for (const EndpointRecord &stored : *active_edges) {
          auto e = stored;
          edge_approximate = approximate || (failed_proof && e.bit_map_approximate);
          if (edge_approximate) {
            e.bit_map_approximate = true;
            SessionRestoreLegacyScalarUnpackedNative(session, e);
          }
          const std::string &e_path = EndpointPath(db, e);
          if ((!edge_approximate || !e.port_query_coverage.empty()) && sig == opts.root_signal && !(domain && e_path == sig) && !root_matches(e)) {
            record_stop(e_path, "bit_filter", "endpoint-does-not-overlap-selected-bits", depth);
            continue;
          }
          if (e.port_mapping_constant || e.port_mapping_unresolved) {
            if (e.port_mapping_unresolved) actual_connection = connection_detail(e, "mapping-is-unproven");
            record_stop(e_path, e.port_mapping_constant ? "constant_connection" : "unresolved_connection_mapping",
                        e.port_mapping_constant ? connection_detail(e, "selected-domain-has-no-signal-driver")
                                                : connection_detail(e, "selected-domain-mapping-is-unproven"), depth);
            continue;
          }
          if (!e.compact_port_routes.empty()) {
            actual_connection = connection_detail(e, "connection");
            RouteDomain selected=domain;
            if (!selected && root_id && sig_id==*root_id) selected=root_domain;
            if (!selected) selected=SessionPortOwnerSelection(session,sig_id,{});
            if (!selected) {
              record_stop(e_path,"unresolved_connection_mapping","compact-port-owner-layout-is-unproven",depth);
              continue;
            }
            std::vector<std::pair<uint32_t,OwnerDomain>> targets;
            size_t pieces=0;bool capped=false;
            for (const auto &r : e.compact_port_routes) for (const auto &[low,high] : *selected) {
              const int32_t a=std::max(low,r.owner_low),b=std::min(high,r.owner_high);
              if (a>b) continue;
              if (++pieces>512) {capped=true;break;}
              auto it=std::find_if(targets.begin(),targets.end(),[&](const auto &t){return t.first==r.target_signal_id;});
              if(it==targets.end()){targets.emplace_back(r.target_signal_id,OwnerDomain{});it=std::prev(targets.end());}
              it->second.emplace_back(int32_t(int64_t(r.target_low)+a-r.owner_low),
                                      int32_t(int64_t(r.target_low)+b-r.owner_low));
            }
            if(capped){record_stop(e_path,"unresolved_connection_mapping","compact-port-route-cap",depth);continue;}
            for (auto &[target,ranges] : targets) {
              std::sort(ranges.begin(),ranges.end());size_t n=0;
              for(const auto range:ranges){
                if(n && int64_t(range.first)<=int64_t(ranges[n-1].second)+1)
                  ranges[n-1].second=std::max(ranges[n-1].second,range.second);
                else ranges[n++]=range;
              }
              ranges.resize(n);descend(target,depth+1,cone_depth,ranges);
            }
            continue;
          }
          if (e.kind == EndpointKind::kPort && !e.port_query_coverage.empty()) {
            // A mapped terminal source port has no expandable exact relation.
            // Do not escape its selected domain through unqualified reverse refs.
            record_stop(e_path, "port_boundary", "mapped-terminal-source-port", depth);
            if (!edge_approximate) {
              if (endpoint_allowed(e) && seen_logic.insert(EndpointKey(db, e)).second) logic_endpoints.push_back(e);
              continue;
            }
            // An unproved hop retains A's ordinary port traversal. The known
            // owner coverage selects this failed chunk; it is not an inverse
            // relation that can restrict the legacy parent/native answer.
          }
          if (e.kind != EndpointKind::kPort) {
            if (!endpoint_allowed(e)) {
              record_stop(e_path, "filtered", "expr-filtered", depth);
              continue;
            }
            const std::string key = EndpointKey(db, e);
            // Dedup uses the original bitmap. Routing also keeps the original record;
            // only the root's owning output copy may show a narrowed merged range.
            // Port projections and fallback drivers can carry a different signal's
            // coordinates even when stored or presented on the root record.
            if (seen_logic.insert(key).second)
              logic_endpoints.push_back(sig == opts.root_signal && e_path == sig && active_edges == &edges
                                            ? ClipRootMergedEndpointCopy(e, opts, root_is_member)
                                            : e);
            if (cone_depth + 1 < opts.cone_level) {
              const std::vector<std::string> &next_signals =
                  is_drivers_mode ? e.rhs_signals : e.lhs_signals;
              if (next_signals.empty()) {
                bool expanded = false;
                if (opts.prefer_port_hop) {
                  if (e_path != sig) {
                    std::optional<uint32_t> direct_id = LookupSignalId(session, e_path);
                    if (direct_id.has_value()) {
                      descend(*direct_id, depth + 1, cone_depth + 1, RouteDomain{});
                      expanded = true;
                    }
                  }
                  const std::vector<uint32_t> bridge_refs =
                      SessionBridgeRefs(session, is_drivers_mode, e.path_id);
                  if (!bridge_refs.empty()) {
                    for (uint32_t next_sig_id : bridge_refs) {
                      if (next_sig_id == sig_id) continue;
                      descend(next_sig_id, depth + 1, cone_depth + 1, RouteDomain{});
                      expanded = true;
                    }
                  }
                }
                if (!expanded) record_stop(e_path, "cone_limit", "no-expandable-assignment-context", depth);
              } else {
                for (const std::string &next_sig : next_signals) {
                  if (next_sig == sig) continue;
                  const auto next = LookupSignalId(session, next_sig);
                  if (next) descend(*next, depth + 1, cone_depth + 1, RouteDomain{});
                  else record_stop(next_sig, "missing_signal", "not-in-db", depth);
                }
              }
            } else if (!e.rhs_signals.empty() || !e.lhs_signals.empty()) {
              record_stop(e_path, "cone_limit", "max-cone-level-reached", depth);
            }
            continue;
          }

          if (edge_approximate && endpoint_allowed(e) && seen_ports.insert(EndpointKey(db, e)).second)
            unresolved_ports.push_back(e);
          bool expanded = false;
          if (e_path != sig) {
            std::optional<uint32_t> direct_id = LookupSignalId(session, e_path);
            if (direct_id.has_value()) {
              descend(*direct_id, depth + 1, cone_depth, RouteDomain{});
              expanded = true;
            }
          }

          const std::vector<uint32_t> bridge_refs =
              SessionBridgeRefs(session, is_drivers_mode, e.path_id);
          if (!bridge_refs.empty()) {
            for (uint32_t next_sig_id : bridge_refs) {
              if (next_sig_id == sig_id) continue;
              const auto source_id = LookupSignalId(session, e_path);
              if (!source_id) {
                record_stop(e_path, "unresolved_connection_mapping", "missing-source-declaration", depth);
                continue;
              }
              expanded |= follow_bridge(*source_id, next_sig_id, cone_depth);
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

  walk_signal_name(opts.root_signal, 0, 0);
  result.visited_count = visited_signals.size();
  result.endpoints = logic_endpoints.empty() ? unresolved_ports : logic_endpoints;
  if (!logic_endpoints.empty()) for (const auto &port : unresolved_ports)
    // Exact load ports are terminal sinks, even when another concatenation
    // chunk reaches logic. Ports crossed during traversal were not collected.
    if ((!is_drivers_mode && !port.bit_map_approximate) ||
        (port.bit_map_approximate && (!fallback_port_sources_known ||
        fallback_root_ports.contains(EndpointPath(db,port)) ||
        fallback_port_sources.contains(EndpointPath(db,port))))) result.endpoints.push_back(port);
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

ParseStatus ParseTraceArgs(const std::vector<std::string> &args, std::optional<std::string> *db_path,
                           TraceOptions &opts, bool require_db) {
  opts = TraceOptions{};
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string &arg = args[i];
    if (arg == "-h" || arg == "--help") {
      PrintTraceHelp();
      return ParseStatus::kExitSuccess;
    }
    if (arg == "--db") {
      if (db_path == nullptr) {
        std::cerr << "--db is not accepted in this context\n";
        return ParseStatus::kError;
      }
      if (i + 1 >= args.size()) {
        std::cerr << "Missing value for --db\n";
        return ParseStatus::kError;
      }
      *db_path = args[++i];
      continue;
    }
    if (arg == "--mode") {
      if (i + 1 >= args.size()) {
        std::cerr << "Missing value for --mode\n";
        return ParseStatus::kError;
      }
      opts.mode = args[++i];
      continue;
    }
    if (arg == "--signal") {
      if (i + 1 >= args.size()) {
        std::cerr << "Missing value for --signal\n";
        return ParseStatus::kError;
      }
      opts.signal = args[++i];
      continue;
    }
    if (arg == "--depth") {
      if (i + 1 >= args.size()) {
        std::cerr << "Missing value for --depth\n";
        return ParseStatus::kError;
      }
      if (!ParseUnsignedCliValue("--depth", args[++i], opts.depth_limit)) return ParseStatus::kError;
      continue;
    }
    if (arg == "--cone-level") {
      if (i + 1 >= args.size()) {
        std::cerr << "Missing value for --cone-level\n";
        return ParseStatus::kError;
      }
      const std::string val = args[++i];
      long long parsed = 0;
      try {
        size_t pos = 0;
        parsed = std::stoll(val, &pos, 10);
        if (pos != val.size()) {
          std::cerr << "Invalid --cone-level: " << val << "\n";
          return ParseStatus::kError;
        }
      } catch (...) {
        std::cerr << "Invalid --cone-level: " << val << "\n";
        return ParseStatus::kError;
      }
      if (parsed < 1) {
        std::cerr << "--cone-level must be >= 1\n";
        return ParseStatus::kError;
      }
      opts.cone_level = static_cast<size_t>(parsed);
      continue;
    }
    if (arg == "--max-nodes") {
      if (i + 1 >= args.size()) {
        std::cerr << "Missing value for --max-nodes\n";
        return ParseStatus::kError;
      }
      if (!ParseUnsignedCliValue("--max-nodes", args[++i], opts.max_nodes)) return ParseStatus::kError;
      continue;
    }
    if (arg == "--prefer-port-hop") {
      opts.prefer_port_hop = true;
      continue;
    }
    if (arg == "--include") {
      if (i + 1 >= args.size()) {
        std::cerr << "Missing value for --include\n";
        return ParseStatus::kError;
      }
      std::regex re;
      if (!CompileUserRegex("--include", args[++i], &re)) return ParseStatus::kError;
      opts.include_re = std::move(re);
      continue;
    }
    if (arg == "--exclude") {
      if (i + 1 >= args.size()) {
        std::cerr << "Missing value for --exclude\n";
        return ParseStatus::kError;
      }
      std::regex re;
      if (!CompileUserRegex("--exclude", args[++i], &re)) return ParseStatus::kError;
      opts.exclude_re = std::move(re);
      continue;
    }
    if (arg == "--stop-at") {
      if (i + 1 >= args.size()) {
        std::cerr << "Missing value for --stop-at\n";
        return ParseStatus::kError;
      }
      std::regex re;
      if (!CompileUserRegex("--stop-at", args[++i], &re)) return ParseStatus::kError;
      opts.stop_at_re = std::move(re);
      continue;
    }
    if (arg == "--format") {
      if (i + 1 >= args.size()) {
        std::cerr << "Missing value for --format\n";
        return ParseStatus::kError;
      }
      auto fmt = ParseOutputFormat(args[++i]);
      if (!fmt.has_value()) {
        std::cerr << "Invalid --format (expected text|json)\n";
        return ParseStatus::kError;
      }
      opts.format = *fmt;
      continue;
    }
    std::cerr << "Unknown option: " << arg << "\n";
    return ParseStatus::kError;
  }

  if ((require_db && (db_path == nullptr || !db_path->has_value())) || opts.mode.empty() || opts.signal.empty()) {
    std::cerr << "Missing required args: " << (require_db ? "--db " : "") << "--mode --signal\n";
    return ParseStatus::kError;
  }
  if (opts.mode != "drivers" && opts.mode != "loads") {
    std::cerr << "Invalid --mode: " << opts.mode << " (expected drivers|loads)\n";
    return ParseStatus::kError;
  }
  // Escaped SV names may contain arbitrary brackets. Syntax is checked after
  // exact stored-name resolution, once the DB's names are available.
  if (!ParseSignalQuery(opts.signal, opts.root_signal, opts.signal_select_axes)) {
    opts.root_signal = opts.signal;
    opts.signal_select_axes.clear();
  }
  if (opts.signal_select_axes.size() == 1) opts.signal_select = opts.signal_select_axes.front();
  return ParseStatus::kOk;
}

// Prefer the longest stored name before treating its remaining suffix as selectors.
// This preserves unusual signal names with brackets as well as indexed instances.
std::optional<TraceOptions> ResolveSignalQuery(const TraceSession &session, const TraceOptions &parsed) {
  TraceOptions resolved = parsed;
  if (LookupSignalId(session, parsed.signal)) {
    resolved.root_signal = parsed.signal;
    resolved.signal_select.reset();
    resolved.signal_select_axes.clear();
    return resolved;
  }
  size_t bracket = parsed.signal.rfind('[');
  while (bracket != std::string::npos) {
    const std::string prefix = parsed.signal.substr(0, bracket);
    if (LookupSignalId(session, prefix)) {
      std::string unused;
      std::vector<std::pair<int32_t, int32_t>> axes;
      if (ParseSignalQuery("signal" + parsed.signal.substr(bracket), unused, axes)) {
        resolved.root_signal = prefix;
        resolved.signal_select_axes = std::move(axes);
        resolved.signal_select.reset();
        if (resolved.signal_select_axes.size() == 1)
          resolved.signal_select = resolved.signal_select_axes.front();
        return resolved;
      }
    }
    if (bracket == 0) break;
    bracket = parsed.signal.rfind('[', bracket - 1);
  }
  if (!ParseSignalQuery(parsed.signal, resolved.root_signal, resolved.signal_select_axes))
    return std::nullopt;
  return resolved;
}

int RunTraceWithSession(TraceSession &session, const TraceOptions &parsed_opts) {
  const auto resolved = ResolveSignalQuery(session, parsed_opts);
  if (!resolved) {
    std::cerr << "Invalid --signal syntax: " << parsed_opts.signal
              << " (expected hier.path followed by signed [index] or [left:right] selects)\n";
    return 1;
  }
  TraceOptions opts = *resolved;
  BuildSessionReverseRefs(session);
  if (!LookupSignalId(session, opts.root_signal).has_value()) {
    std::cerr << "Signal not found: " << opts.root_signal << "\n";
    for (const std::string &s : TopSuggestions(session, opts.root_signal, 5)) {
      std::cerr << "  suggestion: " << s << "\n";
    }
    return 2;
  }
  const uint32_t id = *LookupSignalId(session, opts.root_signal);
  opts.coordinate_encoding = session.db.format_version < 6 ? "legacy_unverified" : "flat_bits";
  auto diagnostic = [&](std::string code, std::string message, bool error) {
    opts.diagnostics.push_back({std::move(code), std::move(message), error ? "error" : "warning"});
    const auto &d = opts.diagnostics.back();
    // JSON goes to stdout and diagnostics to stderr. Text errors have one copy.
    if (!error || opts.format == OutputFormat::kJson)
      std::cerr << d.severity << ": " << d.code << ": " << d.message << "\n";
    if (error) {
      if (opts.format == OutputFormat::kJson) PrintTraceJson(session.db, opts, TraceRunResult{});
      else std::cout << d.severity << ": " << d.code << ": " << d.message << "\n";
    }
    return 1;
  };
  const auto coordinate_rows = session.graph->ReadCoordinates();
  size_t lo = 0, hi = coordinate_rows.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (coordinate_rows[mid].signal_id < id) lo = mid + 1;
    else hi = mid;
  }
  const bool has_coordinates = lo < coordinate_rows.size() && coordinate_rows[lo].signal_id == id;
  const GraphSignalCoordinates coordinate = has_coordinates ? coordinate_rows[lo] : GraphSignalCoordinates{};
  if (has_coordinates) {
    opts.coordinate_encoding = "declared_axes";
    const auto axes = session.graph->ReadDeclaredAxes();
    for (size_t axis = coordinate.axis_begin; axis < coordinate.axis_begin + coordinate.axis_count; ++axis)
      opts.declared_axes.push_back(axes[axis]);
  }
  if (session.graph->ReadSignals()[id].parent_signal_id != std::numeric_limits<uint32_t>::max())
    opts.coordinate_encoding = "parent_struct_bits";
  const auto selector = [](int32_t left, int32_t right) {
    return "[" + std::to_string(left) + (left == right ? "" : ":" + std::to_string(right)) + "]";
  };
  auto flat_rewrite = [&]() -> std::string {
    if (opts.signal_select_axes.size() != 1 || opts.declared_axes.empty()) return "";
    const auto selected = opts.signal_select_axes.front();
    if (selected.first != selected.second || selected.first < 0) return "";
    int64_t bit = selected.first;
    std::vector<int32_t> indexes(opts.declared_axes.size());
    // Bit zero is the rightmost element of every packed axis.
    for (size_t axis = opts.declared_axes.size(); axis-- > 0;) {
      const auto &declared = opts.declared_axes[axis];
      if (!(declared.flags & kAxisFixed) || !(declared.flags & kAxisPacked)) return "";
      const int64_t width = std::abs(int64_t(declared.left) - declared.right) + 1;
      indexes[axis] = static_cast<int32_t>(int64_t(declared.right) +
          (declared.left >= declared.right ? bit % width : -(bit % width)));
      bit /= width;
    }
    if (bit != 0) return "";
    std::string query = opts.root_signal;
    for (int32_t index : indexes) query += selector(index, index);
    return " Flattened bit " + std::to_string(selected.first) + " → " + query + ".";
  };
  if (!opts.signal_select_axes.empty()) {
    const bool member = session.graph->ReadSignals()[id].parent_signal_id != std::numeric_limits<uint32_t>::max();
    if (session.db.format_version < 6 || (member && !session.db.member_declared_axes_verified)) {
      // An old DB does not retain declarations. Even an endpoint-free packed
      // array must not silently be presented as a proven scalar vector.
      diagnostic("legacy_dimensions_unverified",
                 "Legacy DB lacks declared dimensions. Selected coordinates may be flattened bits or array axes; rebuild with the current compiler to disambiguate.", false);
      const SignalRecord &record = SessionSignalRecord(session, id);
      auto multi_axis_evidence = [](const std::vector<EndpointRecord> &edges) {
        return std::any_of(edges.begin(), edges.end(), [](const EndpointRecord &endpoint) {
          const auto axes = ParseEndpointAxes(endpoint.bit_map);
          return axes && axes->size() > 1;
        });
      };
      if (opts.signal_select_axes.size() > 1 || multi_axis_evidence(record.drivers) ||
          multi_axis_evidence(record.loads))
        return diagnostic("legacy_multidimensional_select",
                          "Selected signal has multidimensional access evidence but the legacy DB lacks declared-axis metadata. Rebuild the DB with the current compiler.", true);
    } else if (has_coordinates) {
      if (coordinate.flags & kCoordinateUnsupported)
        return diagnostic("unsupported_coordinate_type",
                          "Selected coordinates of " + std::string((coordinate.flags & kCoordinateTerminalAggregate) ?
                          "a packed struct or packed union array" : "a nonnumeric associative array or nonintegral element type") +
                          " remain unsupported in the current DB format. Querying the whole signal still works: " + opts.root_signal, true);
      if (opts.signal_select_axes.size() > opts.declared_axes.size())
        return diagnostic("too_many_axes", "Query supplies more axes than the signal declares.", true);
      for (size_t axis = 0; axis < opts.signal_select_axes.size(); ++axis) {
        const auto &declared = opts.declared_axes[axis];
        const auto &selected = opts.signal_select_axes[axis];
        if ((declared.flags & kAxisFixed) &&
            (std::min(selected.first, selected.second) < std::min(declared.left, declared.right) ||
             std::max(selected.first, selected.second) > std::max(declared.left, declared.right)))
          return diagnostic("axis_out_of_bounds", "Axis " + std::to_string(axis) + " selection is outside declared [" +
                            std::to_string(declared.left) + ":" + std::to_string(declared.right) +
                            "]. Flattened waveform bit indices are not declared array coordinates." + flat_rewrite(), true);
      }
      if (opts.signal_select_axes.size() == 1 && coordinate.axis_count > 1 && (coordinate.flags & kCoordinatePackedOuter)) {
        const auto selected = opts.signal_select_axes.front();
        std::string row_query = opts.root_signal + selector(selected.first, selected.second);
        for (size_t axis = 1; axis < opts.declared_axes.size(); ++axis)
          row_query += selector(opts.declared_axes[axis].left, opts.declared_axes[axis].right);
        std::string examples = selected.first == selected.second
            ? " Declared row " + std::to_string(selected.first)
            : " Declared row range " + selector(selected.first, selected.second);
        examples += " → " + row_query + "." + flat_rewrite();
        return diagnostic("ambiguous_single_axis",
                          "A single-axis query on a packed multidimensional signal can mean a flattened waveform bit or a declared outer axis. Supply multiple declared axes or query the whole signal; implicit flattened-bit interpretation is unsupported." + examples +
                          (member ? " Struct-member multi-axis selects remain unsupported; query the whole member signal." : ""), true);
      }
    } else if (opts.signal_select_axes.size() > 1 &&
               session.graph->ReadSignals()[id].parent_signal_id == std::numeric_limits<uint32_t>::max()) {
      return diagnostic("unsupported_coordinate_type", "Signal has no supported multidimensional declaration; multidimensional selects remain unsupported.", true);
    }
    if (member && opts.signal_select_axes.size() == 1 && !has_coordinates) {
      const auto &field = session.graph->ReadSignals()[id];
      const auto &selected = opts.signal_select_axes.front();
      const int64_t low = std::min(selected.first, selected.second);
      const int64_t high = std::max(selected.first, selected.second);
      if (low < 0 || high >= field.member_bit_width)
        return diagnostic("axis_out_of_bounds", "Member bit selection is outside the member's stored width.", true);
      if (high + field.member_bit_offset > std::numeric_limits<int32_t>::max())
        return diagnostic("unsupported_coordinate_type", "Member selection exceeds the supported parent bit coordinates.", true);
    }
  }
  if (opts.signal_select_axes.size() > 1) {
    if (session.graph->ReadSignals()[id].parent_signal_id != std::numeric_limits<uint32_t>::max()) {
      return diagnostic("unsupported_struct_member_axes", "Multi-dimensional selects of struct members remain unsupported: " + opts.root_signal, true);
    }
    if (session.db.global_nets.contains(opts.root_signal) ||
        session.db.global_sink_to_source.contains(opts.root_signal)) {
      return diagnostic("unsupported_compact_global_axes", "Multi-dimensional selects of compact global nets remain unsupported: " + opts.root_signal, true);
    }
  }
  TraceRunResult result = RunTraceQuery(session, opts);
  MaterializeAssignmentTexts(session, result);
  if (opts.format == OutputFormat::kJson) {
    PrintTraceJson(session.db, opts, result);
  } else {
    PrintTraceText(session.db, opts, result);
    for (const auto &d : opts.diagnostics)
      std::cout << d.severity << ": " << d.code << ": " << d.message << "\n";
  }
  return 0;
}

int RunTrace(int argc, char *argv[]) {
  std::optional<std::string> db_path;
  TraceOptions opts;
  ParseStatus status = ParseTraceArgs(ArgvToVector(argc, argv), &db_path, opts, true);
  if (status == ParseStatus::kExitSuccess) return 0;
  if (status == ParseStatus::kError) return 1;
  TraceSession session;
  std::string error;
  if (!OpenTraceSession(*db_path, session, kSessionReverseRefs, &error)) {
    std::cerr << error << "\n";
    return 1;
  }
  return RunTraceWithSession(session, opts);
}

} // namespace rtl_trace
