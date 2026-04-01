#pragma once
// HierQuery.h — Hierarchy subcommand interface.
#include "db/GraphDbTypes.h"

namespace rtl_trace {

int RunHierWithSession(TraceSession &session, const HierOptions &opts_in);
ParseStatus ParseHierArgs(const std::vector<std::string> &args,
                          std::optional<std::string> *db_path,
                          HierOptions &opts, bool require_db);

} // namespace rtl_trace
