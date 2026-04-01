// HierQuery.cc — Hierarchy query subcommand implementation.
#include "query/HierQuery.h"
#include "db/GraphDb.h"
#include "db/GraphDbTypes.h"
#include "db/GraphDbInternals.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

namespace rtl_trace {

std::vector<std::string> HierRoots(const TraceDb &db) {
  std::vector<std::string> roots;
  for (const auto &[path, _] : db.hierarchy) {
    const std::string parent = std::string(ParentPath(path));
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
    node.source_file = it->second.source_file;
    node.source_line = it->second.source_line;
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

void PrintHierTreeText(const TraceDb &db, const HierTreeNode &node, size_t indent, bool show_source) {
  std::string lead(indent * 2, ' ');
  std::cout << lead << LeafName(node.path);
  if (!node.module.empty()) std::cout << " (module=" << node.module << ")";
  if (show_source && !node.source_file.empty()) {
    std::cout << " @ " << ResolveSourcePath(db, node.source_file) << ":" << node.source_line;
  }
  std::cout << "\n";
  for (const auto &child : node.children) {
    PrintHierTreeText(db, child, indent + 1, show_source);
  }
}

void PrintHierText(const TraceDb &db, const HierRunResult &result, bool show_source) {
  std::cout << "root: " << result.root << "\n";
  std::cout << "depth: " << result.depth_limit << "\n";
  std::cout << "nodes: " << result.node_count << "\n";
  if (result.truncated) std::cout << "truncated: true\n";
  if (result.tree.has_value()) {
    std::cout << "\n";
    PrintHierTreeText(db, *result.tree, 0, show_source);
  }
  if (!result.stops.empty()) {
    std::cout << "stops: " << result.stops.size() << "\n";
    for (const std::string &s : result.stops) {
      std::cout << "  stop  " << s << "\n";
    }
  }
}

void PrintHierTreeJson(const TraceDb &db, const HierTreeNode &node, bool show_source) {
  std::cout << "{\"path\":\"" << JsonEscape(node.path) << "\",\"module\":\"" << JsonEscape(node.module)
            << "\"";
  if (show_source && !node.source_file.empty()) {
    std::cout << ",\"source\":{\"file\":\"" << JsonEscape(ResolveSourcePath(db, node.source_file))
              << "\",\"line\":" << node.source_line << "}";
  }
  std::cout << ",\"children\":[";
  for (size_t i = 0; i < node.children.size(); ++i) {
    if (i) std::cout << ",";
    PrintHierTreeJson(db, node.children[i], show_source);
  }
  std::cout << "]}";
}

void PrintHierJson(const TraceDb &db, const HierRunResult &result, bool show_source) {
  std::cout << "{\"root\":\"" << JsonEscape(result.root) << "\",\"depth_limit\":" << result.depth_limit
            << ",\"node_count\":" << result.node_count
            << ",\"truncated\":" << (result.truncated ? "true" : "false") << ",\"tree\":";
  if (result.tree.has_value()) {
    PrintHierTreeJson(db, *result.tree, show_source);
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

ParseStatus ParseHierArgs(const std::vector<std::string> &args, std::optional<std::string> *db_path,
                          HierOptions &opts, bool require_db) {
  opts = HierOptions{};
  opts.root.clear();
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string &arg = args[i];
    if (arg == "-h" || arg == "--help") {
      PrintHierHelp();
      return ParseStatus::kExitSuccess;
    }
    if (arg == "--db") {
      if (db_path == nullptr) {
        std::cerr << "--db is not accepted in this context\n";
        return ParseStatus::kError;
      }
      if (i + 1 >= args.size()) return std::cerr << "Missing value for --db\n", ParseStatus::kError;
      *db_path = args[++i];
      continue;
    }
    if (arg == "--root") {
      if (i + 1 >= args.size()) return std::cerr << "Missing value for --root\n", ParseStatus::kError;
      opts.root = args[++i];
      continue;
    }
    if (arg == "--depth") {
      if (i + 1 >= args.size()) return std::cerr << "Missing value for --depth\n", ParseStatus::kError;
      if (!ParseUnsignedCliValue("--depth", args[++i], opts.depth_limit)) return ParseStatus::kError;
      continue;
    }
    if (arg == "--max-nodes") {
      if (i + 1 >= args.size()) return std::cerr << "Missing value for --max-nodes\n", ParseStatus::kError;
      if (!ParseUnsignedCliValue("--max-nodes", args[++i], opts.max_nodes)) return ParseStatus::kError;
      continue;
    }
    if (arg == "--format") {
      if (i + 1 >= args.size()) return std::cerr << "Missing value for --format\n", ParseStatus::kError;
      auto fmt = ParseOutputFormat(args[++i]);
      if (!fmt.has_value()) return std::cerr << "Invalid --format (expected text|json)\n", ParseStatus::kError;
      opts.format = *fmt;
      continue;
    }
    if (arg == "--show-source") {
      opts.show_source = true;
      continue;
    }
    return std::cerr << "Unknown option: " << arg << "\n", ParseStatus::kError;
  }
  if (require_db && (db_path == nullptr || !db_path->has_value())) {
    return std::cerr << "Missing required args: --db\n", ParseStatus::kError;
  }
  return ParseStatus::kOk;
}

int RunHierWithSession(TraceSession &session, const HierOptions &opts_in) {
  EnsureSessionHierarchy(session);
  HierOptions opts = opts_in;
  if (session.db.hierarchy.empty()) return std::cerr << "No hierarchy data in DB\n", 1;
  if (opts.root.empty()) {
    const auto roots = HierRoots(session.db);
    if (roots.empty()) return std::cerr << "No hierarchy roots found in DB\n", 1;
    opts.root = roots.front();
  }
  if (session.db.hierarchy.find(opts.root) == session.db.hierarchy.end()) {
    std::cerr << "Root not found: " << opts.root << "\n";
    for (const std::string &s : TopHierarchySuggestions(session.db, opts.root, 5)) {
      std::cerr << "  suggestion: " << s << "\n";
    }
    return 2;
  }
  HierRunResult result = RunHierQuery(session.db, opts);
  if (opts.format == OutputFormat::kJson) {
    PrintHierJson(session.db, result, opts.show_source);
  } else {
    PrintHierText(session.db, result, opts.show_source);
  }
  return 0;
}

int RunHier(int argc, char *argv[]) {
  std::optional<std::string> db_path;
  HierOptions opts;
  ParseStatus status = ParseHierArgs(ArgvToVector(argc, argv), &db_path, opts, true);
  if (status == ParseStatus::kExitSuccess) return 0;
  if (status == ParseStatus::kError) return 1;
  TraceSession session;
  if (!OpenTraceSession(*db_path, session, kSessionHierarchy)) {
    std::cerr << "Failed to read DB: " << *db_path << "\n";
    return 1;
  }
  return RunHierWithSession(session, opts);
}

} // namespace rtl_trace
