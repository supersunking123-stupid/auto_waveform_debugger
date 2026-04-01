#pragma once
// GraphDb.h — Public API for standalone_trace modules.

namespace rtl_trace {

int RunCompile(int argc, char *argv[]);
int RunTrace(int argc, char *argv[]);
int RunHier(int argc, char *argv[]);
int RunWhereInstance(int argc, char *argv[]);
int RunFind(int argc, char *argv[]);
int RunServe(int argc, char *argv[]);

} // namespace rtl_trace
