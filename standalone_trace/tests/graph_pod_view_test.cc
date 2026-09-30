#include "db/GraphDbTypes.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

using namespace rtl_trace;

static void Require(bool condition) {
  if (!condition) std::abort();
}

template <typename T>
static void CheckUnalignedRecords() {
  std::array<T, 3> records;
  for (size_t i = 0; i < records.size(); ++i)
    std::memset(&records[i], static_cast<int>(i + 1), sizeof(T));
  std::vector<char> bytes(sizeof(records) + 1);
  std::memcpy(bytes.data() + 1, records.data(), sizeof(records));
  GraphPodView<T> mapped(bytes.data() + 1, records.size());
  Require(mapped.size() == records.size() && !mapped.empty());
  size_t i = 0;
  for (T record : mapped) {
    Require(std::memcmp(&record, &records[i], sizeof(T)) == 0);
    ++i;
  }
  Require(i == records.size());
  for (i = 0; i < records.size(); ++i) {
    const T record = mapped[i];
    Require(std::memcmp(&record, &records[i], sizeof(T)) == 0);
  }
  const std::vector<T> copy(mapped.begin(), mapped.end());
  GraphPodView<T> owned(copy);
  const T last = owned[2];
  Require(std::memcmp(&last, &records[2], sizeof(T)) == 0);
  const std::vector<T> slice(mapped.begin() + 1, mapped.begin() + 3);
  Require(slice.size() == 2);
  Require(std::memcmp(&slice[0], &records[1], sizeof(T)) == 0);
  GraphPodView<T> empty;
  Require(empty.empty() && empty.begin() == empty.end());
  Require(empty.begin() + 0 == empty.end());
}

int main() {
  CheckUnalignedRecords<uint32_t>();
  CheckUnalignedRecords<GraphSignalRecord>();
  CheckUnalignedRecords<GraphEndpointRecord>();
  CheckUnalignedRecords<GraphPathRefRange>();
  CheckUnalignedRecords<GraphHierarchyRecord>();
  CheckUnalignedRecords<GraphHierarchyRecordV2>();
  CheckUnalignedRecords<GraphGlobalNetRecord>();
  CheckUnalignedRecords<GraphInstanceParamRecord>();
  std::cout << "unaligned mapped POD views: all record types passed\n";
}
