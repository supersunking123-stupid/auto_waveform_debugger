#pragma once
// WhereIsQuery.h — WhereIs-instance subcommand interface.
#include "db/GraphDbTypes.h"

namespace rtl_trace {

int RunWhereInstanceWithSession(TraceSession &session, const WhereInstanceOptions &opts);
ParseStatus ParseWhereInstanceArgs(const std::vector<std::string> &args,
                                   std::optional<std::string> *db_path,
                                   WhereInstanceOptions &opts, bool require_db);

} // namespace rtl_trace
