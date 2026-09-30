#pragma once
// TraceQuery.h — Trace subcommand interface.
#include "db/GraphDbTypes.h"

#include <regex>
#include <string>

namespace rtl_trace {

// Compiles a user-supplied ECMAScript regex. On a malformed pattern prints
// "Invalid regex for <option>: '<pattern>' (<reason>)" to stderr and returns false, so
// callers can report ParseStatus::kError instead of letting std::regex_error abort the
// process (or a resident `serve` session). `out` may be null to only validate.
bool CompileUserRegex(const char *option, const std::string &pattern, std::regex *out);

int RunTraceWithSession(TraceSession &session, const TraceOptions &opts);
ParseStatus ParseTraceArgs(const std::vector<std::string> &args,
                           std::optional<std::string> *db_path,
                           TraceOptions &opts, bool require_db);

} // namespace rtl_trace
