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

std::string InstanceParameterKindName(InstanceParameterKind kind) {
  return kind == InstanceParameterKind::kType ? "type" : "value";
}

std::optional<std::string> SourceUnavailableReason(const TraceDb &db,
                                                   const WhereInstanceResult &result) {
  if (!result.source_file.empty()) return std::nullopt;
  if (db.format_version != 0 && db.format_version < 3) {
    return "db version " + std::to_string(db.format_version) +
           " does not store hierarchy source metadata; rebuild with current rtl_trace compile";
  }
  return "no source metadata recorded for this instance definition";
}

std::optional<std::string> ParamsUnavailableReason(const TraceDb &db) {
  if (db.format_version != 0 && db.format_version < 4) {
    return "db version " + std::to_string(db.format_version) +
           " does not store instance parameter metadata; rebuild with current rtl_trace compile";
  }
  return std::nullopt;
}

std::optional<WhereInstanceResult> LookupWhereInstance(const TraceDb &db, const std::string &instance) {
  const auto it = db.hierarchy.find(instance);
  if (it == db.hierarchy.end()) return std::nullopt;
  WhereInstanceResult result;
  result.instance = instance;
  result.module = it->second.module;
  result.source_file = it->second.source_file;
  result.source_line = it->second.source_line;
  result.parameters = it->second.parameters;
  return result;
}

void PrintWhereInstanceText(const TraceDb &db, const WhereInstanceResult &result, bool show_params) {
  std::cout << "instance: " << result.instance << "\n";
  std::cout << "module: " << result.module << "\n";
  if (!result.source_file.empty()) {
    std::cout << "source: " << ResolveSourcePath(db, result.source_file) << ":" << result.source_line << "\n";
  } else {
    std::cout << "source: <unavailable>";
    if (const auto reason = SourceUnavailableReason(db, result); reason.has_value()) {
      std::cout << " (" << *reason << ")";
    }
    std::cout << "\n";
  }
  if (!show_params) return;
  if (const auto reason = ParamsUnavailableReason(db); reason.has_value()) {
    std::cout << "parameters: <unavailable> (" << *reason << ")\n";
    return;
  }
  std::cout << "parameters: " << result.parameters.size() << "\n";
  for (const InstanceParameterRecord &param : result.parameters) {
    std::cout << "  param " << InstanceParameterKindName(param.kind) << " " << param.name
              << " = " << param.value;
    std::vector<std::string> attrs;
    if (param.is_port) attrs.push_back("port");
    if (param.is_local) attrs.push_back("local");
    if (param.is_overridden) attrs.push_back("overridden");
    if (!attrs.empty()) {
      std::cout << " [";
      for (size_t i = 0; i < attrs.size(); ++i) {
        if (i) std::cout << ",";
        std::cout << attrs[i];
      }
      std::cout << "]";
    }
    std::cout << "\n";
  }
}

void PrintWhereInstanceJson(const TraceDb &db, const WhereInstanceResult &result, bool show_params) {
  std::cout << "{\"instance\":\"" << JsonEscape(result.instance) << "\",\"module\":\""
            << JsonEscape(result.module) << "\"";
  if (!result.source_file.empty()) {
    std::cout << ",\"source\":{\"file\":\"" << JsonEscape(ResolveSourcePath(db, result.source_file))
              << "\",\"line\":" << result.source_line << "}";
  } else {
    std::cout << ",\"source\":null";
    if (const auto reason = SourceUnavailableReason(db, result); reason.has_value()) {
      std::cout << ",\"source_unavailable_reason\":\"" << JsonEscape(*reason) << "\"";
    }
  }
  if (show_params) {
    if (const auto reason = ParamsUnavailableReason(db); reason.has_value()) {
      std::cout << ",\"parameters\":null"
                << ",\"parameters_unavailable_reason\":\"" << JsonEscape(*reason) << "\"";
    } else {
      std::cout << ",\"parameters\":[";
      for (size_t i = 0; i < result.parameters.size(); ++i) {
        if (i) std::cout << ",";
        const InstanceParameterRecord &param = result.parameters[i];
        std::cout << "{\"name\":\"" << JsonEscape(param.name)
                  << "\",\"kind\":\"" << JsonEscape(InstanceParameterKindName(param.kind))
                  << "\",\"value\":\"" << JsonEscape(param.value)
                  << "\",\"is_local\":" << (param.is_local ? "true" : "false")
                  << ",\"is_port\":" << (param.is_port ? "true" : "false")
                  << ",\"is_overridden\":" << (param.is_overridden ? "true" : "false") << "}";
      }
      std::cout << "]";
    }
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
    if (arg == "--show-params") {
      opts.show_params = true;
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
    PrintWhereInstanceJson(session.db, *result, opts.show_params);
  } else {
    PrintWhereInstanceText(session.db, *result, opts.show_params);
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
