#pragma once
// FindQuery.h — Find subcommand interface.
#include "db/GraphDbTypes.h"

namespace rtl_trace {

int RunFindWithSession(TraceSession &session, const FindOptions &opts);
ParseStatus ParseFindArgs(const std::vector<std::string> &args,
                          std::optional<std::string> *db_path,
                          FindOptions &opts, bool require_db);

} // namespace rtl_trace
