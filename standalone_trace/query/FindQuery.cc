// FindQuery.cc — Find query subcommand implementation.
#include "query/FindQuery.h"
#include "db/EntryPoints.h"
#include "db/GraphDbTypes.h"
#include "db/GraphDbInternals.h"
#include "db/ParallelTopK.h"

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

namespace {

// Longest run of adjacent, mandatory literal characters in an ECMAScript regex, or "" when none
// can be proven. Every string matched by `pattern` must contain the returned run as a substring,
// so it is a safe prefilter before std::regex_search. Only simple patterns are analyzed: any
// alternation, group, bracket expression, brace quantifier, or escape other than an escaped
// punctuation character makes the analysis give up (empty result).
std::string RequiredLiteral(const std::string &pattern) {
  static const std::string kEscapable = ".^$*+?()[]{}|/\\";
  std::string best, run;
  auto end_run = [&]() {
    if (run.size() > best.size()) best = run;
    run.clear();
  };
  const size_t n = pattern.size();
  size_t i = 0;
  while (i < n) {
    const char c = pattern[i];
    char lit = 0;
    size_t next = i + 1;
    if (c == '|' || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}') return "";
    if (c == '\\') {
      if (i + 1 >= n || kEscapable.find(pattern[i + 1]) == std::string::npos) return "";
      lit = pattern[i + 1];
      next = i + 2;
    } else if (c == '.' || c == '^' || c == '$' || c == '*' || c == '+' || c == '?') {
      // Any-char, assertion, or stray quantifier/lazy modifier: nothing adjacent is provable.
      end_run();
      i = next;
      continue;
    } else {
      lit = c;
    }
    const char q = next < n ? pattern[next] : '\0';
    if (q == '*' || q == '?') {
      end_run(); // optional literal: not required, and it breaks adjacency
    } else if (q == '+') {
      run.push_back(lit); // required at least once, but repetition breaks adjacency after it
      end_run();
    } else {
      run.push_back(lit);
    }
    i = next;
  }
  end_run();
  return best;
}

} // namespace

int RunFindWithSession(TraceSession &session, const FindOptions &opts) {
  std::optional<std::regex> re;
  if (opts.regex_mode) re = std::regex(opts.query);
  const std::string literal = opts.regex_mode ? RequiredLiteral(opts.query) : std::string();

  // Parallel bounded top-k: equals "all matches, sorted, truncated to --limit".
  const std::vector<const std::string *> &names = session.signal_names_by_id;
  auto name_less = [](const std::string *a, const std::string *b) { return *a < *b; };
  using Collector = TopKCollector<const std::string *, decltype(name_less)>;
  const std::vector<const std::string *> top = ParallelTopK<const std::string *>(
      names.size(), opts.limit, name_less, [&](size_t begin, size_t end, Collector &out) {
        for (size_t i = begin; i < end; ++i) {
          const std::string &name = *names[i];
          bool ok;
          if (opts.regex_mode) {
            ok = (literal.empty() || name.find(literal) != std::string::npos) &&
                 std::regex_search(name, *re);
          } else {
            ok = (name.find(opts.query) != std::string::npos);
          }
          if (ok) out.Push(names[i]);
        }
      });
  std::vector<std::string> matches;
  matches.reserve(top.size());
  for (const std::string *m : top) matches.push_back(*m);

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
