// FindQuery.cc — Find query subcommand implementation.
#include "query/FindQuery.h"
#include "db/GraphDb.h"
#include "db/GraphDbTypes.h"
#include "db/GraphDbInternals.h"

#include <algorithm>
#include <iostream>
#include <regex>
#include <string>
#include <vector>

namespace rtl_trace {

ParseStatus ParseFindArgs(const std::vector<std::string> &args, std::optional<std::string> *db_path,
                          FindOptions &opts, bool require_db) {
  opts = FindOptions{};
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string &arg = args[i];
    if (arg == "-h" || arg == "--help") {
      PrintFindHelp();
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
    if (arg == "--query") {
      if (i + 1 >= args.size()) return std::cerr << "Missing value for --query\n", ParseStatus::kError;
      opts.query = args[++i];
      continue;
    }
    if (arg == "--regex") {
      opts.regex_mode = true;
      continue;
    }
    if (arg == "--limit") {
      if (i + 1 >= args.size()) return std::cerr << "Missing value for --limit\n", ParseStatus::kError;
      if (!ParseUnsignedCliValue("--limit", args[++i], opts.limit)) return ParseStatus::kError;
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
  if ((require_db && (db_path == nullptr || !db_path->has_value())) || opts.query.empty()) {
    return std::cerr << "Missing required args: " << (require_db ? "--db " : "") << "--query\n",
           ParseStatus::kError;
  }
  return ParseStatus::kOk;
}

int RunFindWithSession(TraceSession &session, const FindOptions &opts) {
  std::vector<std::string> matches;
  std::optional<std::regex> re;
  if (opts.regex_mode) re = std::regex(opts.query);
  for (const std::string *name : session.signal_names_by_id) {
    bool ok = false;
    if (opts.regex_mode) {
      ok = std::regex_search(*name, *re);
    } else {
      ok = (name->find(opts.query) != std::string::npos);
    }
    if (ok) matches.push_back(*name);
  }
  std::sort(matches.begin(), matches.end());
  if (matches.size() > opts.limit) matches.resize(opts.limit);

  std::vector<std::string> suggestions;
  if (matches.empty()) suggestions = TopSuggestions(session, opts.query, opts.limit);

  if (opts.format == OutputFormat::kJson) {
    std::cout << "{\"query\":\"" << JsonEscape(opts.query) << "\",\"regex\":"
              << (opts.regex_mode ? "true" : "false") << ",\"count\":" << matches.size() << ",\"matches\":[";
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
    std::cout << "query: " << opts.query << "\n";
    std::cout << "regex: " << (opts.regex_mode ? "true" : "false") << "\n";
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

int RunFind(int argc, char *argv[]) {
  std::optional<std::string> db_path;
  FindOptions opts;
  ParseStatus status = ParseFindArgs(ArgvToVector(argc, argv), &db_path, opts, true);
  if (status == ParseStatus::kExitSuccess) return 0;
  if (status == ParseStatus::kError) return 1;
  TraceSession session;
  if (!OpenTraceSession(*db_path, session, kSessionSignals)) {
    std::cerr << "Failed to read DB: " << *db_path << "\n";
    return 1;
  }
  return RunFindWithSession(session, opts);
}

} // namespace rtl_trace
