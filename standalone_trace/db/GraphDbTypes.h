#pragma once
// GraphDbTypes.h — Shared type definitions for standalone_trace modules.
// This header is self-contained: no slang AST headers, only slang/util/Hash.h.

#include "slang/util/Hash.h"
#include "slang/util/FlatMap.h"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace rtl_trace {

// --- Runtime types (used by all modules) ---

enum class EndpointKind { kPort, kExpr };

struct EndpointRecord {
  EndpointKind kind = EndpointKind::kExpr;
  std::string path;
  std::string file;
  uint32_t path_id = std::numeric_limits<uint32_t>::max();
  uint32_t file_id = std::numeric_limits<uint32_t>::max();
  int line = 0;
  std::string direction;
  std::string assignment_text;
  bool has_assignment_range = false;
  uint32_t assignment_start = 0;
  uint32_t assignment_end = 0;
  std::string bit_map;
  bool bit_map_approximate = false;
  std::vector<uint32_t> lhs_signal_ids;
  std::vector<uint32_t> rhs_signal_ids;
  std::vector<std::string> lhs_signals;
  std::vector<std::string> rhs_signals;
};

struct SignalRecord {
  std::vector<EndpointRecord> drivers;
  std::vector<EndpointRecord> loads;
};

enum class InstanceParameterKind : uint8_t { kValue = 0, kType = 1 };

struct InstanceParameterRecord {
  std::string name;
  std::string value;
  InstanceParameterKind kind = InstanceParameterKind::kValue;
  bool is_local = false;
  bool is_port = false;
  bool is_overridden = false;
};

struct HierNodeRecord {
  std::string module;
  std::string source_file;
  uint32_t source_line = 0;
  std::vector<InstanceParameterRecord> parameters;
  std::vector<std::string> children;
};

struct GlobalNetRecord {
  std::string category;
  std::vector<std::string> sinks;
};

struct TraceDb {
  slang::flat_hash_map<std::string, SignalRecord> signals;
  slang::flat_hash_map<std::string, HierNodeRecord> hierarchy;
  slang::flat_hash_map<std::string, GlobalNetRecord> global_nets;
  slang::flat_hash_map<std::string, std::string> global_sink_to_source;
  std::vector<std::string> path_pool;
  std::vector<std::string> file_pool;
  std::string db_dir;
  uint32_t format_version = 0;
};

// --- Graph DB binary format types ---

struct GraphSignalRecord {
  uint32_t name_str_id = std::numeric_limits<uint32_t>::max();
  uint32_t driver_begin = 0;
  uint32_t driver_count = 0;
  uint32_t load_begin = 0;
  uint32_t load_count = 0;
};

struct GraphEndpointRecord {
  uint32_t path_str_id = std::numeric_limits<uint32_t>::max();
  uint32_t file_str_id = std::numeric_limits<uint32_t>::max();
  uint32_t direction_str_id = std::numeric_limits<uint32_t>::max();
  uint32_t bit_map_str_id = std::numeric_limits<uint32_t>::max();
  uint32_t line = 0;
  uint32_t assignment_start = 0;
  uint32_t assignment_end = 0;
  uint32_t lhs_begin = 0;
  uint32_t lhs_count = 0;
  uint32_t rhs_begin = 0;
  uint32_t rhs_count = 0;
  uint8_t kind = 0;
  uint8_t bit_map_approximate = 0;
  uint8_t has_assignment_range = 0;
  uint8_t reserved = 0;
};

struct GraphPathRefRange {
  uint32_t path_str_id = std::numeric_limits<uint32_t>::max();
  uint32_t begin = 0;
  uint32_t count = 0;
};

struct GraphHierarchyRecord {
  uint32_t path_str_id = std::numeric_limits<uint32_t>::max();
  uint32_t module_str_id = std::numeric_limits<uint32_t>::max();
  uint32_t file_str_id = std::numeric_limits<uint32_t>::max();
  uint32_t line = 0;
  uint32_t child_begin = 0;
  uint32_t child_count = 0;
};

struct GraphHierarchyRecordV2 {
  uint32_t path_str_id = std::numeric_limits<uint32_t>::max();
  uint32_t module_str_id = std::numeric_limits<uint32_t>::max();
  uint32_t child_begin = 0;
  uint32_t child_count = 0;
};

struct GraphGlobalNetRecord {
  uint32_t source_path_str_id = std::numeric_limits<uint32_t>::max();
  uint32_t category_str_id = std::numeric_limits<uint32_t>::max();
  uint32_t sink_begin = 0;
  uint32_t sink_count = 0;
};

struct GraphInstanceParamRecord {
  uint32_t name_str_id = std::numeric_limits<uint32_t>::max();
  uint32_t value_str_id = std::numeric_limits<uint32_t>::max();
  uint8_t kind = 0;
  uint8_t is_local = 0;
  uint8_t is_port = 0;
  uint8_t is_overridden = 0;
};

struct GraphDb {
  std::vector<std::string> strings;
  std::vector<GraphSignalRecord> signals;
  std::vector<GraphEndpointRecord> endpoints;
  std::vector<uint32_t> signal_refs;
  std::vector<GraphPathRefRange> load_ref_ranges;
  std::vector<uint32_t> load_ref_signal_ids;
  std::vector<GraphPathRefRange> driver_ref_ranges;
  std::vector<uint32_t> driver_ref_signal_ids;
  std::vector<GraphPathRefRange> assignment_lhs_ref_ranges;
  std::vector<uint32_t> assignment_lhs_ref_signal_ids;
  std::vector<GraphHierarchyRecord> hierarchy;
  std::vector<uint32_t> hierarchy_children;
  std::vector<GraphPathRefRange> hierarchy_param_ranges;
  std::vector<GraphInstanceParamRecord> hierarchy_params;
  std::vector<GraphGlobalNetRecord> global_nets;
  std::vector<uint32_t> global_sinks;
  slang::flat_hash_map<uint32_t, size_t> load_ref_index;
  slang::flat_hash_map<uint32_t, size_t> driver_ref_index;
  slang::flat_hash_map<uint32_t, size_t> assignment_lhs_ref_index;
};

// --- Session types ---

struct TraceSession {
  TraceDb db;
  std::optional<GraphDb> graph;
  std::string db_path;
  std::string db_mtime;
  slang::flat_hash_map<std::string_view, uint32_t> signal_name_to_id;
  std::vector<const std::string *> signal_names_by_id;
  slang::flat_hash_map<uint32_t, SignalRecord> materialized_signal_records;
  slang::flat_hash_map<std::string, std::string> source_file_cache;
  bool signal_index_ready = false;
  bool reverse_refs_ready = false;
  bool hierarchy_ready = false;
};

struct PartitionRecord {
  std::string root;
  size_t signal_count = 0;
  size_t depth = 0;
};

enum SessionBuildFlags : uint32_t {
  kSessionSignals = 1u << 0,
  kSessionHierarchy = 1u << 1,
  kSessionReverseRefs = 1u << 2,
};

// --- Query option/result types ---

enum class OutputFormat { kText, kJson };

struct TraceOptions {
  std::string mode;
  std::string signal;
  std::string root_signal;
  std::optional<std::pair<int32_t, int32_t>> signal_select;
  size_t cone_level = 1;
  bool prefer_port_hop = false;
  size_t depth_limit = 8;
  size_t max_nodes = 5000;
  std::optional<std::regex> include_re;
  std::optional<std::regex> exclude_re;
  std::optional<std::regex> stop_at_re;
  OutputFormat format = OutputFormat::kText;
};

struct TraceStop {
  std::string signal;
  std::string reason;
  std::string detail;
  size_t depth = 0;
};

struct TraceRunResult {
  std::vector<EndpointRecord> endpoints;
  std::vector<TraceStop> stops;
  size_t visited_count = 0;
};

struct HierOptions {
  std::string root;
  size_t depth_limit = 8;
  size_t max_nodes = 5000;
  OutputFormat format = OutputFormat::kText;
  bool show_source = false;
};

struct WhereInstanceOptions {
  std::string instance;
  OutputFormat format = OutputFormat::kText;
  bool show_params = false;
};

struct FindOptions {
  std::string query;
  bool regex_mode = false;
  size_t limit = 20;
  OutputFormat format = OutputFormat::kText;
};

struct HierTreeNode {
  std::string path;
  std::string module;
  std::string source_file;
  uint32_t source_line = 0;
  std::vector<HierTreeNode> children;
};

struct HierRunResult {
  std::string root;
  size_t depth_limit = 0;
  size_t node_count = 0;
  bool truncated = false;
  std::vector<std::string> stops;
  std::optional<HierTreeNode> tree;
};

struct WhereInstanceResult {
  std::string instance;
  std::string module;
  std::string source_file;
  uint32_t source_line = 0;
  std::vector<InstanceParameterRecord> parameters;
};

enum class ParseStatus { kOk, kExitSuccess, kError };

// --- Binary I/O header constants ---

constexpr size_t kGraphDbMagicSize = 16;
constexpr char kGraphDbMagic[kGraphDbMagicSize] = {
    'R', 'T', 'L', '_', 'T', 'R', 'A', 'C', 'E', '_', 'G', 'D', 'B', '_', '1', '\0'};

struct GraphDbFileHeader {
  char magic[kGraphDbMagicSize];
  uint32_t version = 4;
  uint32_t reserved = 0;
  uint64_t string_count = 0;
  uint64_t string_blob_size = 0;
  uint64_t signal_count = 0;
  uint64_t endpoint_count = 0;
  uint64_t signal_ref_count = 0;
  uint64_t load_ref_range_count = 0;
  uint64_t load_ref_count = 0;
  uint64_t driver_ref_range_count = 0;
  uint64_t driver_ref_count = 0;
  uint64_t assignment_lhs_ref_range_count = 0;
  uint64_t assignment_lhs_ref_count = 0;
  uint64_t hierarchy_count = 0;
  uint64_t hierarchy_child_count = 0;
  uint64_t global_net_count = 0;
  uint64_t global_sink_count = 0;
};

// --- Binary I/O templates (header-only) ---

template <typename T>
bool WriteBinaryValue(std::ofstream &out, const T &value) {
  out.write(reinterpret_cast<const char *>(&value), sizeof(T));
  return out.good();
}

template <typename T>
bool ReadBinaryValue(std::ifstream &in, T &value) {
  in.read(reinterpret_cast<char *>(&value), sizeof(T));
  return in.good();
}

template <typename T>
bool WriteBinaryVector(std::ofstream &out, const std::vector<T> &items) {
  if (items.empty()) return true;
  out.write(reinterpret_cast<const char *>(items.data()),
            static_cast<std::streamsize>(items.size() * sizeof(T)));
  return out.good();
}

template <typename T>
bool ReadBinaryVector(std::ifstream &in, std::vector<T> &items, size_t count) {
  items.resize(count);
  if (count == 0) return true;
  in.read(reinterpret_cast<char *>(items.data()),
          static_cast<std::streamsize>(items.size() * sizeof(T)));
  return in.good();
}

// CompileLogger — used by compile module
class CompileLogger {
 public:
  explicit CompileLogger(const std::string &log_path) {
    if (!log_path.empty()) file_.open(log_path, std::ios::out | std::ios::trunc);
  }

  void Log(const std::string &msg) {
    const std::string line = "[rtl_trace] " + Timestamp() + " " + msg;
    std::cerr << line << "\n";
    if (file_.is_open()) {
      file_ << line << "\n";
      file_.flush();
    }
  }

 private:
  static std::string Timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    std::ostringstream os;
    os << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
    return os.str();
  }

  std::ofstream file_;
};

} // namespace rtl_trace
