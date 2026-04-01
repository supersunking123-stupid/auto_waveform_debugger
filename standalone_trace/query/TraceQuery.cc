// TraceQuery.cc — Trace query subcommand implementation.
#include "query/TraceQuery.h"
#include "db/GraphDb.h"
#include "db/GraphDbTypes.h"
#include "db/GraphDbInternals.h"

#include <iostream>
#include <regex>
#include <string>
#include <unordered_set>
#include <vector>

namespace rtl_trace {

std::vector<EndpointRecord> FindFallbackDriverEndpoints(TraceSession &session, uint32_t target_sig_id) {
  std::vector<EndpointRecord> out;
  if (!session.graph.has_value()) return out;

  const GraphDb &graph = *session.graph;
  if (target_sig_id >= graph.signals.size()) return out;

  const uint32_t path_id = graph.signals[target_sig_id].name_str_id;
  const std::string &target_signal = SessionSignalName(session, target_sig_id);
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

TraceRunResult RunTraceQuery(TraceSession &session, const TraceOptions &opts) {
  const TraceDb &db = session.db;
  if (std::optional<TraceRunResult> fast = TryRunGlobalNetFastPath(db, opts)) {
    return *fast;
  }
  TraceRunResult result;

  const bool is_drivers_mode = (opts.mode == "drivers");
  std::vector<EndpointRecord> logic_endpoints;
  std::vector<EndpointRecord> unresolved_ports;
  std::unordered_set<std::string> seen_logic;
  std::unordered_set<std::string> seen_ports;
  std::unordered_set<uint32_t> visited_signals;
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

  std::function<void(uint32_t, size_t, size_t)> walk_signal;
  std::function<void(std::string_view, size_t, size_t)> walk_signal_name =
      [&](std::string_view sig_name, size_t depth, size_t cone_depth) {
        std::optional<uint32_t> sig_id = LookupSignalId(session, sig_name);
        if (!sig_id.has_value()) {
          record_stop(std::string(sig_name), "missing_signal", "not-in-db", depth);
          return;
        }
        walk_signal(*sig_id, depth, cone_depth);
      };

  walk_signal = [&](uint32_t sig_id, size_t depth, size_t cone_depth) {
        const std::string &sig = SessionSignalName(session, sig_id);
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
        if (!visited_signals.insert(sig_id).second) {
          record_stop(sig, "cycle", "already-visited", depth);
          return;
        }
        if (opts.stop_at_re.has_value() && std::regex_search(sig, *opts.stop_at_re)) {
          record_stop(sig, "stop_at", "matched-stop-at-regex", depth);
          return;
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

        for (const EndpointRecord &e : *active_edges) {
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
                  if (e_path != sig) {
                    std::optional<uint32_t> direct_id = LookupSignalId(session, e_path);
                    if (direct_id.has_value()) {
                      walk_signal(*direct_id, depth + 1, cone_depth + 1);
                      expanded = true;
                    }
                  }
                  const std::vector<uint32_t> bridge_refs =
                      SessionBridgeRefs(session, is_drivers_mode, e.path_id);
                  if (!bridge_refs.empty()) {
                    for (uint32_t next_sig_id : bridge_refs) {
                      if (next_sig_id == sig_id) continue;
                      walk_signal(next_sig_id, depth + 1, cone_depth + 1);
                      expanded = true;
                    }
                  }
                }
                if (!expanded) record_stop(e_path, "cone_limit", "no-expandable-assignment-context", depth);
              } else {
                for (const std::string &next_sig : next_signals) {
                  if (next_sig == sig) continue;
                  walk_signal_name(next_sig, depth + 1, cone_depth + 1);
                }
              }
            } else if (!e.rhs_signals.empty() || !e.lhs_signals.empty()) {
              record_stop(e_path, "cone_limit", "max-cone-level-reached", depth);
            }
            continue;
          }

          bool expanded = false;
          if (e_path != sig) {
            std::optional<uint32_t> direct_id = LookupSignalId(session, e_path);
            if (direct_id.has_value()) {
              walk_signal(*direct_id, depth + 1, cone_depth);
              expanded = true;
            }
          }

          const std::vector<uint32_t> bridge_refs =
              SessionBridgeRefs(session, is_drivers_mode, e.path_id);
          if (!bridge_refs.empty()) {
            for (uint32_t next_sig_id : bridge_refs) {
              if (next_sig_id == sig_id) continue;
              walk_signal(next_sig_id, depth + 1, cone_depth);
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

  walk_signal_name(opts.root_signal, 0, 0);
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
      opts.include_re = std::regex(args[++i]);
      continue;
    }
    if (arg == "--exclude") {
      if (i + 1 >= args.size()) {
        std::cerr << "Missing value for --exclude\n";
        return ParseStatus::kError;
      }
      opts.exclude_re = std::regex(args[++i]);
      continue;
    }
    if (arg == "--stop-at") {
      if (i + 1 >= args.size()) {
        std::cerr << "Missing value for --stop-at\n";
        return ParseStatus::kError;
      }
      opts.stop_at_re = std::regex(args[++i]);
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
  if (!ParseSignalQuery(opts.signal, opts.root_signal, opts.signal_select)) {
    std::cerr << "Invalid --signal syntax: " << opts.signal
              << " (expected hier.path or hier.path[bit] or hier.path[msb:lsb])\n";
    return ParseStatus::kError;
  }
  return ParseStatus::kOk;
}

int RunTraceWithSession(TraceSession &session, const TraceOptions &opts) {
  if (!LookupSignalId(session, opts.root_signal).has_value()) {
    std::cerr << "Signal not found: " << opts.root_signal << "\n";
    for (const std::string &s : TopSuggestions(session, opts.root_signal, 5)) {
      std::cerr << "  suggestion: " << s << "\n";
    }
    return 2;
  }
  TraceRunResult result = RunTraceQuery(session, opts);
  MaterializeAssignmentTexts(session, result);
  if (opts.format == OutputFormat::kJson) {
    PrintTraceJson(session.db, opts, result);
  } else {
    PrintTraceText(session.db, opts, result);
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
  if (!OpenTraceSession(*db_path, session, kSessionReverseRefs)) {
    std::cerr << "Failed to read DB: " << *db_path << "\n";
    return 1;
  }
  return RunTraceWithSession(session, opts);
}

} // namespace rtl_trace
