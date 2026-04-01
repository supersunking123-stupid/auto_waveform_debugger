// WhereIsQuery.cc — WhereIs-instance query subcommand implementation.
#include "query/WhereIsQuery.h"
#include "db/EntryPoints.h"
#include "db/GraphDbTypes.h"
#include "db/GraphDbInternals.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace rtl_trace {

std::optional<WhereInstanceResult> LookupWhereInstance(const TraceDb &db, const std::string &instance) {
  const auto it = db.hierarchy.find(instance);
  if (it == db.hierarchy.end()) return std::nullopt;
  WhereInstanceResult result;
  result.instance = instance;
  result.module = it->second.module;
  result.source_file = it->second.source_file;
  result.source_line = it->second.source_line;
  return result;
}

void PrintWhereInstanceText(const TraceDb &db, const WhereInstanceResult &result) {
  std::cout << "instance: " << result.instance << "\n";
  std::cout << "module: " << result.module << "\n";
  if (!result.source_file.empty()) {
    std::cout << "source: " << ResolveSourcePath(db, result.source_file) << ":" << result.source_line << "\n";
  } else {
    std::cout << "source: <unavailable>\n";
  }
}

void PrintWhereInstanceJson(const TraceDb &db, const WhereInstanceResult &result) {
  std::cout << "{\"instance\":\"" << JsonEscape(result.instance) << "\",\"module\":\""
            << JsonEscape(result.module) << "\"";
  if (!result.source_file.empty()) {
    std::cout << ",\"source\":{\"file\":\"" << JsonEscape(ResolveSourcePath(db, result.source_file))
              << "\",\"line\":" << result.source_line << "}";
  } else {
    std::cout << ",\"source\":null";
  }
  std::cout << "}\n";
}

ParseStatus ParseWhereInstanceArgs(const std::vector<std::string> &args,
                                   std::optional<std::string> *db_path,
                                   WhereInstanceOptions &opts, bool require_db) {
  opts = WhereInstanceOptions{};
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string &arg = args[i];
    if (arg == "-h" || arg == "--help") {
      PrintWhereInstanceHelp();
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
    if (arg == "--instance") {
      if (i + 1 >= args.size()) return std::cerr << "Missing value for --instance\n", ParseStatus::kError;
      opts.instance = args[++i];
      continue;
    }
    if (arg == "--format") {
      if (i + 1 >= args.size()) return std::cerr << "Missing value for --format\n", ParseStatus::kError;
      auto fmt = ParseOutputFormat(args[++i]);
      if (!fmt.has_value()) return std::cerr << "Invalid --format (expected text|json)\n", ParseStatus::kError;
      opts.format = *fmt;
      continue;
    }
    return std::cerr << "Unknown option: " << arg << "\n", ParseStatus::kError;
  }
  if ((require_db && (db_path == nullptr || !db_path->has_value())) || opts.instance.empty()) {
    return std::cerr << "Missing required args: " << (require_db ? "--db " : "") << "--instance\n",
           ParseStatus::kError;
  }
  return ParseStatus::kOk;
}

int RunWhereInstanceWithSession(TraceSession &session, const WhereInstanceOptions &opts) {
  EnsureSessionHierarchy(session);
  if (session.db.hierarchy.empty()) return std::cerr << "No hierarchy data in DB\n", 1;
  std::optional<WhereInstanceResult> result = LookupWhereInstance(session.db, opts.instance);
  if (!result.has_value()) {
    std::cerr << "Instance not found: " << opts.instance << "\n";
    for (const std::string &s : TopHierarchySuggestions(session.db, opts.instance, 5)) {
      std::cerr << "  suggestion: " << s << "\n";
    }
    return 2;
  }
  if (opts.format == OutputFormat::kJson) {
    PrintWhereInstanceJson(session.db, *result);
  } else {
    PrintWhereInstanceText(session.db, *result);
  }
  return 0;
}

int RunWhereInstance(int argc, char *argv[]) {
  std::optional<std::string> db_path;
  WhereInstanceOptions opts;
  ParseStatus status = ParseWhereInstanceArgs(ArgvToVector(argc, argv), &db_path, opts, true);
  if (status == ParseStatus::kExitSuccess) return 0;
  if (status == ParseStatus::kError) return 1;
  TraceSession session;
  if (!OpenTraceSession(*db_path, session, kSessionHierarchy)) {
    std::cerr << "Failed to read DB: " << *db_path << "\n";
    return 1;
  }
  return RunWhereInstanceWithSession(session, opts);
}

} // namespace rtl_trace
