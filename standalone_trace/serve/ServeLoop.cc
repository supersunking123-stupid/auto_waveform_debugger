// ServeLoop.cc — Interactive serve mode implementation.
#include "serve/ServeLoop.h"
#include "db/EntryPoints.h"
#include "db/GraphDbTypes.h"
#include "db/GraphDbInternals.h"
#include "query/TraceQuery.h"
#include "query/HierQuery.h"
#include "query/FindQuery.h"
#include "query/WhereIsQuery.h"

#include <iostream>
#include <string>
#include <cctype>
#include <vector>

namespace rtl_trace {

bool SplitCommandLine(const std::string &line, std::vector<std::string> &out, std::string &err) {
  out.clear();
  err.clear();
  std::string cur;
  bool in_single = false;
  bool in_double = false;
  bool escaped = false;
  for (char c : line) {
    if (escaped) {
      cur.push_back(c);
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      continue;
    }
    if (in_single) {
      if (c == '\'') {
        in_single = false;
      } else {
        cur.push_back(c);
      }
      continue;
    }
    if (in_double) {
      if (c == '"') {
        in_double = false;
      } else {
        cur.push_back(c);
      }
      continue;
    }
    if (c == '\'') {
      in_single = true;
      continue;
    }
    if (c == '"') {
      in_double = true;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
      continue;
    }
    cur.push_back(c);
  }
  if (escaped || in_single || in_double) {
    err = "unterminated escape or quote";
    return false;
  }
  if (!cur.empty()) out.push_back(cur);
  return true;
}

int RunServe(int argc, char *argv[]) {
  std::optional<std::string> startup_db;
  for (int i = 0; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      PrintServeHelp();
      return 0;
    }
    if (arg == "--db") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --db\n";
        return 1;
      }
      startup_db = argv[++i];
      continue;
    }
    std::cerr << "Unknown option: " << arg << "\n";
    return 1;
  }

  std::optional<TraceSession> session;
  auto finish_response = []() {
    std::cout << "<<END>>\n";
    std::cout.flush();
  };
  auto open_db = [&](const std::string &path) -> int {
    TraceSession loaded;
    if (!OpenTraceSession(path, loaded, kSessionHierarchy | kSessionReverseRefs)) {
      std::cerr << "Failed to read DB: " << path << "\n";
      return 1;
    }
    session = std::move(loaded);
    std::cout << "db: " << session->db_path << "\n";
    std::cout << "signals: " << session->signal_names_by_id.size() << "\n";
    std::cout << "hier_nodes: " << session->db.hierarchy.size() << "\n";
    return 0;
  };

  if (startup_db.has_value()) {
    (void)open_db(*startup_db);
  } else {
    std::cout << "db: <none>\n";
  }
  finish_response();

  std::string line;
  while (std::getline(std::cin, line)) {
    std::vector<std::string> tokens;
    std::string split_err;
    if (!SplitCommandLine(line, tokens, split_err)) {
      std::cerr << "Command parse error: " << split_err << "\n";
      finish_response();
      continue;
    }
    if (tokens.empty()) {
      finish_response();
      continue;
    }
    if (tokens[0].size() > 0 && tokens[0][0] == '#') {
      finish_response();
      continue;
    }

    const std::string cmd = tokens[0];
    std::vector<std::string> args(tokens.begin() + 1, tokens.end());

    if (cmd == "quit" || cmd == "exit") {
      std::cout << "bye\n";
      finish_response();
      break;
    }
    if (cmd == "help") {
      if (args.empty()) {
        PrintServeHelp();
      } else if (args[0] == "trace") {
        PrintTraceHelp();
      } else if (args[0] == "find") {
        PrintFindHelp();
      } else if (args[0] == "hier") {
        PrintHierHelp();
      } else if (args[0] == "whereis-instance") {
        PrintWhereInstanceHelp();
      } else if (args[0] == "serve") {
        PrintServeHelp();
      } else {
        std::cerr << "Unknown help topic: " << args[0] << "\n";
      }
      finish_response();
      continue;
    }
    if (cmd == "status") {
      if (!session.has_value()) {
        std::cout << "db: <none>\n";
      } else {
        std::cout << "db: " << session->db_path << "\n";
        std::cout << "mtime: " << session->db_mtime << "\n";
        std::cout << "signals: " << session->signal_names_by_id.size() << "\n";
        std::cout << "hier_nodes: " << session->db.hierarchy.size() << "\n";
        const size_t load_ref_paths = session->graph->load_ref_ranges.size();
        const size_t driver_ref_paths = session->graph->driver_ref_ranges.size();
        std::cout << "reverse_ref_paths_loads: " << load_ref_paths << "\n";
        std::cout << "reverse_ref_paths_drivers: " << driver_ref_paths << "\n";
      }
      finish_response();
      continue;
    }
    if (cmd == "close") {
      session.reset();
      std::cout << "db: <none>\n";
      finish_response();
      continue;
    }
    if (cmd == "reload") {
      if (!session.has_value()) {
        std::cerr << "No DB is open\n";
      } else {
        (void)open_db(session->db_path);
      }
      finish_response();
      continue;
    }
    if (cmd == "open") {
      std::optional<std::string> db_path;
      for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--db") {
          if (i + 1 >= args.size()) {
            std::cerr << "Missing value for --db\n";
            db_path.reset();
            break;
          }
          db_path = args[++i];
        } else if (args[i].rfind("--", 0) == 0) {
          std::cerr << "Unknown option: " << args[i] << "\n";
          db_path.reset();
          break;
        } else if (!db_path.has_value()) {
          db_path = args[i];
        } else {
          std::cerr << "Unexpected extra argument: " << args[i] << "\n";
          db_path.reset();
          break;
        }
      }
      if (!db_path.has_value()) {
        if (args.empty()) std::cerr << "Missing DB path\n";
      } else {
        (void)open_db(*db_path);
      }
      finish_response();
      continue;
    }

    if (!session.has_value()) {
      std::cerr << "No DB is open\n";
      finish_response();
      continue;
    }

    if (cmd == "trace") {
      TraceOptions opts;
      ParseStatus status = ParseTraceArgs(args, nullptr, opts, false);
      if (status == ParseStatus::kOk) (void)RunTraceWithSession(*session, opts);
      finish_response();
      continue;
    }
    if (cmd == "find") {
      FindOptions opts;
      ParseStatus status = ParseFindArgs(args, nullptr, opts, false);
      if (status == ParseStatus::kOk) (void)RunFindWithSession(*session, opts);
      finish_response();
      continue;
    }
    if (cmd == "hier") {
      HierOptions opts;
      ParseStatus status = ParseHierArgs(args, nullptr, opts, false);
      if (status == ParseStatus::kOk) (void)RunHierWithSession(*session, opts);
      finish_response();
      continue;
    }
    if (cmd == "whereis-instance") {
      WhereInstanceOptions opts;
      ParseStatus status = ParseWhereInstanceArgs(args, nullptr, opts, false);
      if (status == ParseStatus::kOk) (void)RunWhereInstanceWithSession(*session, opts);
      finish_response();
      continue;
    }

    std::cerr << "Unknown command: " << cmd << "\n";
    finish_response();
  }
  return 0;
}

} // namespace rtl_trace
