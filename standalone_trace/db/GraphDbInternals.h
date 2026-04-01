#pragma once
// GraphDbInternals.h — Internal function declarations shared across standalone_trace modules.
// This header has NO slang AST dependencies — only standard library + GraphDbTypes.h.

#include "db/GraphDbTypes.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace rtl_trace {

// --- Memory utilities ---

long GetMaxRSSMB();
long GetCurrentRSSMB();
void LogMem(const std::string &step);

// --- String / path utilities ---

std::string_view ParentPath(std::string_view path);
std::string_view LeafName(std::string_view path);
std::pair<std::string_view, std::string_view> SplitPathPrefixLeaf(std::string_view path);

uint32_t InternString(const std::string &s, std::vector<std::string> &pool,
                      slang::flat_hash_map<std::string, uint32_t> &index);
const std::string &GraphString(const GraphDb &db, uint32_t id);
const std::string &EndpointPath(const TraceDb &db, const EndpointRecord &e);
const std::string &EndpointFile(const TraceDb &db, const EndpointRecord &e);

std::vector<std::string> SplitJoinedField(const std::string &field);
std::string JsonEscape(std::string_view s);
size_t EditDistance(const std::string &a, const std::string &b);
std::string ToLower(std::string s);
bool LooksLikeOptionToken(const std::string &s);
std::string ToAbsPathString(const std::filesystem::path &p, const std::filesystem::path &base);

// --- Bit / range utilities ---

std::optional<std::pair<int32_t, int32_t>> ParseExactBitMapText(std::string_view bit_map);
std::string FormatBitRange(int32_t hi, int32_t lo);
bool TryParseSimpleInt(std::string_view s, int64_t &out);
bool RegexMatch(const std::optional<std::regex> &re, const std::string &s);
bool ParseSignalQuery(const std::string &input, std::string &base_signal,
                      std::optional<std::pair<int32_t, int32_t>> &bit_select);
std::optional<std::pair<int32_t, int32_t>> ParseExactBitRange(const EndpointRecord &e);
bool RangesOverlap(const std::pair<int32_t, int32_t> &a, const std::pair<int32_t, int32_t> &b);
bool EndpointMatchesSignalSelect(const EndpointRecord &e,
                                  const std::optional<std::pair<int32_t, int32_t>> &select);
std::string EndpointKey(const TraceDb &db, const EndpointRecord &e);

// --- Global net ---

bool LooksLikeClockOrResetName(std::string_view path);

// --- Session management ---

std::string StatMtimeString(const std::string &path);
void BuildSessionSignalIndex(TraceSession &session);
void EnsureSessionHierarchy(TraceSession &session);
void BuildSessionReverseRefs(TraceSession &session);
bool OpenTraceSession(const std::string &db_path, TraceSession &session, uint32_t flags);
std::optional<uint32_t> LookupSignalId(const TraceSession &session, std::string_view name);
const std::string &SessionSignalName(const TraceSession &session, uint32_t id);
const SignalRecord &SessionSignalRecord(TraceSession &session, uint32_t id);
std::vector<uint32_t> SessionBridgeRefs(const TraceSession &session, bool use_load_refs,
                                         uint32_t path_id);
std::vector<uint32_t> SessionAssignmentLhsRefs(const TraceSession &session, uint32_t path_id);

// --- Graph DB I/O (runtime) ---

bool ValidateGraphRange(uint32_t begin, uint32_t count, size_t size);
bool ValidateGraphDb(const GraphDb &graph);
bool LoadGraphDb(const std::string &db_path, GraphDb &graph, TraceDb &compat_db);

// --- CLI utilities ---

bool HasTimescaleArg(const std::vector<std::string> &args);
std::optional<OutputFormat> ParseOutputFormat(const std::string &s);
bool ParseUnsignedCliValue(const std::string &flag, const std::string &value, size_t &out);
std::vector<std::string> ArgvToVector(int argc, char *argv[]);
bool HasTopArg(const std::vector<std::string> &args);
void CollectFilesFromFlist(const std::filesystem::path &flist,
                            std::vector<std::filesystem::path> &files);
bool ParsePlusList(std::string_view tok, std::string_view prefix,
                   std::vector<std::string> &out, const std::filesystem::path &base);
bool ParseDefinesPlus(std::string_view tok, std::vector<std::string> &out);
std::string ComputeCompileFingerprint(const std::vector<std::string> &passthrough_args);

// --- Help printers ---

void PrintGeneralHelp();
void PrintTraceHelp();
void PrintHierHelp();
void PrintWhereInstanceHelp();
void PrintFindHelp();
void PrintServeHelp();

// --- Query output helpers (trace) ---

std::vector<std::string> TopSuggestions(const TraceSession &session,
                                         const std::string &needle, size_t limit);
std::string ResolveSourcePath(const TraceDb &db, const std::string &file);
std::string FetchSourceSlice(TraceSession &session, const EndpointRecord &e);
std::string ResolveHierarchySourcePath(const TraceDb &db, const HierNodeRecord &node);
std::vector<std::string> InferAssignmentLhsPaths(TraceSession &session, EndpointRecord &e);
void MaterializeAssignmentTexts(TraceSession &session, TraceRunResult &result);
void PrintTraceText(const TraceDb &db, const TraceOptions &opts, const TraceRunResult &result);
void PrintTraceJson(const TraceDb &db, const TraceOptions &opts, const TraceRunResult &result);

// --- Query output helpers (hierarchy) ---

std::vector<std::string> HierRoots(const TraceDb &db);
std::vector<std::string> TopHierarchySuggestions(const TraceDb &db,
                                                   const std::string &needle, size_t limit);
HierRunResult RunHierQuery(const TraceDb &db, const HierOptions &opts);
void PrintHierText(const TraceDb &db, const HierRunResult &result, bool show_source);
void PrintHierJson(const TraceDb &db, const HierRunResult &result, bool show_source);

// --- Query output helpers (whereis-instance) ---

std::optional<WhereInstanceResult> LookupWhereInstance(const TraceDb &db,
                                                        const std::string &instance);
void PrintWhereInstanceText(const TraceDb &db, const WhereInstanceResult &result);
void PrintWhereInstanceJson(const TraceDb &db, const WhereInstanceResult &result);

} // namespace rtl_trace
