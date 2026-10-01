#pragma once
// GraphDbTypes.h — Shared type definitions for standalone_trace modules.
// This header is self-contained: no slang AST headers, only slang/util/Hash.h.

#include "slang/util/Hash.h"
#include "slang/util/FlatMap.h"

#include <chrono>
#include <cstring>
#include <iterator>
#include <memory>
#include <type_traits>
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
enum class SourcePathMode : uint8_t { kLogical = 0, kPhysicalAbsolute = 1 };

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
  bool bit_map_logical_axes = false;
  bool bit_map_merged = false;
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
  bool member_declared_axes_verified = false;
};

// --- Graph DB binary format types ---

struct GraphSignalRecord {
  uint32_t name_str_id = std::numeric_limits<uint32_t>::max();
  uint32_t driver_begin = 0;
  uint32_t driver_count = 0;
  uint32_t load_begin = 0;
  uint32_t load_count = 0;
  // Level 2: struct member metadata (zeroed for non-member signals and v4 DBs)
  uint32_t parent_signal_id = std::numeric_limits<uint32_t>::max();
  uint32_t member_bit_offset = 0;
  uint32_t member_bit_width = 0;
};

constexpr uint8_t kEndpointMergedRange = 0x01;
constexpr uint8_t kEndpointLogicalAxes = 0x02;
constexpr uint32_t kCoordinatePackedOuter = 0x01;
constexpr uint32_t kDbDeclaredAxes = 0x01;
constexpr uint32_t kDbMemberDeclaredAxes = 0x02;
constexpr uint32_t kCoordinateUnsupported = 0x02;
constexpr uint32_t kCoordinateTerminalEnum = 0x04;
constexpr uint32_t kCoordinateTerminalAggregate = 0x08;
constexpr uint32_t kAxisFixed = 0x01;
constexpr uint32_t kAxisPacked = 0x02;
struct GraphSignalCoordinates {
  uint32_t signal_id = 0;
  uint32_t axis_begin = 0;
  uint32_t axis_count = 0;
  uint32_t flags = 0;
};
struct GraphDeclaredAxis {
  int32_t left = 0;
  int32_t right = 0;
  uint32_t flags = 0;
};
static_assert(sizeof(GraphSignalCoordinates) == 16);
static_assert(sizeof(GraphDeclaredAxis) == 12);

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

// Read-only POD view. v1-v5 files do not align the section after the string blob.
// Returning records by value via memcpy avoids unaligned loads and object-lifetime UB.
template <typename T>
class GraphPodView {
  static_assert(std::is_trivially_copyable_v<T>);
 public:
  GraphPodView() = default;
  GraphPodView(const char *data, size_t count) : data_(data), size_(count) {}
  explicit GraphPodView(const std::vector<T> &items)
      : data_(reinterpret_cast<const char *>(items.data())), size_(items.size()) {}
  size_t size() const { return size_; }
  const char *data() const { return data_; }
  bool empty() const { return size_ == 0; }
  T operator[](size_t i) const {
    T value;
    std::memcpy(&value, data_ + i * sizeof(T), sizeof(T));
    return value;
  }
  class Iterator {
   public:
    using iterator_category = std::input_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = void;
    using reference = T;
    Iterator() = default;
    explicit Iterator(const char *data) : data_(data) {}
    T operator*() const {
      T value;
      std::memcpy(&value, data_, sizeof(T));
      return value;
    }
    Iterator &operator++() { data_ += sizeof(T); return *this; }
    Iterator operator++(int) { auto old = *this; ++*this; return old; }
    Iterator operator+(size_t n) const { return Iterator(n == 0 ? data_ : data_ + n * sizeof(T)); }
    bool operator==(const Iterator &other) const { return data_ == other.data_; }
   private:
    const char *data_ = nullptr;
  };
  Iterator begin() const { return Iterator(data_); }
  Iterator end() const { return Iterator(size_ == 0 ? data_ : data_ + size_ * sizeof(T)); }
 private:
  const char *data_ = nullptr;
  size_t size_ = 0;
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
  std::vector<GraphSignalCoordinates> coordinates;
  std::vector<GraphDeclaredAxis> declared_axes;
  GraphPodView<GraphSignalCoordinates> mapped_coordinates;
  GraphPodView<GraphDeclaredAxis> mapped_declared_axes;
  GraphPodView<GraphSignalCoordinates> ReadCoordinates() const {
    return mapped_coordinates.empty() ? GraphPodView<GraphSignalCoordinates>(coordinates) : mapped_coordinates;
  }
  GraphPodView<GraphDeclaredAxis> ReadDeclaredAxes() const {
    return mapped_declared_axes.empty() ? GraphPodView<GraphDeclaredAxis>(declared_axes) : mapped_declared_axes;
  }
  // Compiler writes use the owned vectors above. Runtime reads use immutable mappings.
  std::shared_ptr<void> mapping;
  size_t mapping_bytes = 0;
  int mapping_fd = -1;  // owned by mapping's deleter, never closed separately
  GraphPodView<uint32_t> mapped_string_offsets;
  const char *mapped_string_blob = nullptr;
  size_t mapped_string_blob_bytes = 0;
  GraphPodView<GraphSignalRecord> mapped_signals;
  GraphPodView<GraphSignalRecord> ReadSignals() const {
    return mapped_signals.empty() ? GraphPodView<GraphSignalRecord>(signals) : mapped_signals;
  }
  GraphPodView<GraphEndpointRecord> mapped_endpoints;
  GraphPodView<GraphEndpointRecord> ReadEndpoints() const {
    return mapped_endpoints.empty() ? GraphPodView<GraphEndpointRecord>(endpoints) : mapped_endpoints;
  }
  GraphPodView<uint32_t> mapped_signal_refs;
  GraphPodView<uint32_t> ReadSignalRefs() const {
    return mapped_signal_refs.empty() ? GraphPodView<uint32_t>(signal_refs) : mapped_signal_refs;
  }
  GraphPodView<GraphPathRefRange> mapped_load_ref_ranges;
  GraphPodView<GraphPathRefRange> ReadLoadRefRanges() const {
    return mapped_load_ref_ranges.empty() ? GraphPodView<GraphPathRefRange>(load_ref_ranges) : mapped_load_ref_ranges;
  }
  GraphPodView<uint32_t> mapped_load_ref_signal_ids;
  GraphPodView<uint32_t> ReadLoadRefSignalIds() const {
    return mapped_load_ref_signal_ids.empty() ? GraphPodView<uint32_t>(load_ref_signal_ids) : mapped_load_ref_signal_ids;
  }
  GraphPodView<GraphPathRefRange> mapped_driver_ref_ranges;
  GraphPodView<GraphPathRefRange> ReadDriverRefRanges() const {
    return mapped_driver_ref_ranges.empty() ? GraphPodView<GraphPathRefRange>(driver_ref_ranges) : mapped_driver_ref_ranges;
  }
  GraphPodView<uint32_t> mapped_driver_ref_signal_ids;
  GraphPodView<uint32_t> ReadDriverRefSignalIds() const {
    return mapped_driver_ref_signal_ids.empty() ? GraphPodView<uint32_t>(driver_ref_signal_ids) : mapped_driver_ref_signal_ids;
  }
  GraphPodView<GraphPathRefRange> mapped_assignment_lhs_ref_ranges;
  GraphPodView<GraphPathRefRange> ReadAssignmentLhsRefRanges() const {
    return mapped_assignment_lhs_ref_ranges.empty() ? GraphPodView<GraphPathRefRange>(assignment_lhs_ref_ranges) : mapped_assignment_lhs_ref_ranges;
  }
  GraphPodView<uint32_t> mapped_assignment_lhs_ref_signal_ids;
  GraphPodView<uint32_t> ReadAssignmentLhsRefSignalIds() const {
    return mapped_assignment_lhs_ref_signal_ids.empty() ? GraphPodView<uint32_t>(assignment_lhs_ref_signal_ids) : mapped_assignment_lhs_ref_signal_ids;
  }
  GraphPodView<GraphHierarchyRecord> mapped_hierarchy;
  GraphPodView<GraphHierarchyRecord> ReadHierarchy() const {
    return mapped_hierarchy.empty() ? GraphPodView<GraphHierarchyRecord>(hierarchy) : mapped_hierarchy;
  }
  GraphPodView<uint32_t> mapped_hierarchy_children;
  GraphPodView<uint32_t> ReadHierarchyChildren() const {
    return mapped_hierarchy_children.empty() ? GraphPodView<uint32_t>(hierarchy_children) : mapped_hierarchy_children;
  }
  GraphPodView<GraphPathRefRange> mapped_hierarchy_param_ranges;
  GraphPodView<GraphPathRefRange> ReadHierarchyParamRanges() const {
    return mapped_hierarchy_param_ranges.empty() ? GraphPodView<GraphPathRefRange>(hierarchy_param_ranges) : mapped_hierarchy_param_ranges;
  }
  GraphPodView<GraphInstanceParamRecord> mapped_hierarchy_params;
  GraphPodView<GraphInstanceParamRecord> ReadHierarchyParams() const {
    return mapped_hierarchy_params.empty() ? GraphPodView<GraphInstanceParamRecord>(hierarchy_params) : mapped_hierarchy_params;
  }
  GraphPodView<GraphGlobalNetRecord> mapped_global_nets;
  GraphPodView<GraphGlobalNetRecord> ReadGlobalNets() const {
    return mapped_global_nets.empty() ? GraphPodView<GraphGlobalNetRecord>(global_nets) : mapped_global_nets;
  }
  GraphPodView<uint32_t> mapped_global_sinks;
  GraphPodView<uint32_t> ReadGlobalSinks() const {
    return mapped_global_sinks.empty() ? GraphPodView<uint32_t>(global_sinks) : mapped_global_sinks;
  }
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
  std::vector<std::string_view> signal_names_by_id;
  bool signal_names_ready = false;
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
  std::vector<std::pair<int32_t, int32_t>> signal_select_axes;
  size_t cone_level = 1;
  bool prefer_port_hop = false;
  size_t depth_limit = 8;
  size_t max_nodes = 5000;
  std::optional<std::regex> include_re;
  std::optional<std::regex> exclude_re;
  std::optional<std::regex> stop_at_re;
  OutputFormat format = OutputFormat::kText;
  std::string coordinate_encoding = "flat_bits";
  std::vector<GraphDeclaredAxis> declared_axes;
  struct Diagnostic { std::string code, message, severity; };
  std::vector<Diagnostic> diagnostics;
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
  uint32_t version = 6;
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
