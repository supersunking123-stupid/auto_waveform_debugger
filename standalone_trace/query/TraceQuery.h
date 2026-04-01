#pragma once
// TraceQuery.h — Trace subcommand interface.
#include "db/GraphDbTypes.h"

namespace rtl_trace {

int RunTraceWithSession(TraceSession &session, const TraceOptions &opts);
ParseStatus ParseTraceArgs(const std::vector<std::string> &args,
                           std::optional<std::string> *db_path,
                           TraceOptions &opts, bool require_db);

} // namespace rtl_trace
