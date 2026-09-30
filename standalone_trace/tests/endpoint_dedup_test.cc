#include "db/EndpointDedup.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
using namespace rtl_trace;

void Require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

struct ConstantHash {
  size_t operator()(const EndpointFullKey &) const { return 0; }
};

EndpointRecord Record(std::string path) {
  EndpointRecord e;
  e.path = std::move(path);
  e.file = "fixture.sv";
  e.path_id = 7;
  e.file_id = 8;
  e.line = 12;
  e.direction = "input";
  e.assignment_text = "x = y";
  e.assignment_start = 11;
  e.assignment_end = 20;
  e.bit_map = "[1][0]";
  e.lhs_signal_ids = {1, 2};
  e.rhs_signal_ids = {3, 4};
  e.lhs_signals = {"t.x", "t.x", "t.y"};
  e.rhs_signals = {"t.a", "t.b"};
  return e;
}

template <typename Hash>
void CheckStableOrder() {
  EndpointDedupScratch<Hash> scratch;
  const auto a = Record("a");
  const auto b = Record(std::string(200, 'b'));
  const auto c = Record("c");
  std::vector<EndpointRecord> entries{a, b, a, c, b};
  const char *first_b_storage = entries[1].path.data();
  Require(DeduplicateEndpointsInPlace(entries, scratch) == 2, "wrong removed count");
  Require(entries.size() == 3 && SameEndpointFields(entries[0], a) &&
          SameEndpointFields(entries[1], b) && SameEndpointFields(entries[2], c), "unstable survivors");
  Require(entries[1].path.data() == first_b_storage, "retained a later occurrence");
  Require(scratch.seen.empty(), "pointer keys survived compaction");
  Require(DeduplicateEndpointsInPlace(entries, scratch) == 0, "unique entries changed");
  entries.assign(1000, b);
  Require(DeduplicateEndpointsInPlace(entries, scratch) == 999 && entries.size() == 1 &&
          SameEndpointFields(entries[0], b), "scratch reuse/all equal failed");
  Require(DeduplicateEndpointsInPlace(entries, scratch) == 0, "singleton changed");
  entries.clear();
  Require(DeduplicateEndpointsInPlace(entries, scratch) == 0, "empty changed");
  SignalRecord signal;
  signal.drivers = {a, a}; signal.loads = {a, a};
  Require(DeduplicateEndpointsInPlace(signal.drivers, scratch) == 1 &&
          DeduplicateEndpointsInPlace(signal.loads, scratch) == 1 &&
          signal.drivers.size() == 1 && signal.loads.size() == 1, "lists deduped across directions");
  scratch.Release();
}

void CheckEveryField() {
  const auto original = Record("a");
  std::vector<EndpointRecord> distinct{original};
  auto change = [&](auto mutation) {
    auto e = original; mutation(e);
    Require(!SameEndpointFields(e, original), "field omitted from full equality");
    distinct.push_back(std::move(e));
  };
  change([](auto &e) { e.kind = EndpointKind::kPort; });
  change([](auto &e) { e.path += "x"; });
  change([](auto &e) { ++e.path_id; });
  change([](auto &e) { e.file += "x"; });
  change([](auto &e) { ++e.file_id; });
  change([](auto &e) { ++e.line; });
  change([](auto &e) { e.direction += "x"; });
  change([](auto &e) { e.assignment_text += "x"; });
  change([](auto &e) { e.has_assignment_range = true; });
  change([](auto &e) { ++e.assignment_start; });  // include bounds even with range=false
  change([](auto &e) { ++e.assignment_end; });
  change([](auto &e) { e.bit_map += "[2]"; });
  change([](auto &e) { e.bit_map_approximate = true; });
  change([](auto &e) { e.bit_map_logical_axes = true; });
  change([](auto &e) { e.bit_map_merged = true; });
  change([](auto &e) { e.lhs_signal_ids.push_back(9); });
  change([](auto &e) { e.rhs_signal_ids.push_back(9); });
  change([](auto &e) { e.lhs_signals.push_back("z"); });
  change([](auto &e) { e.rhs_signals.push_back("z"); });
  change([](auto &e) { std::reverse(e.lhs_signal_ids.begin(), e.lhs_signal_ids.end()); });
  change([](auto &e) { std::reverse(e.rhs_signal_ids.begin(), e.rhs_signal_ids.end()); });
  change([](auto &e) { std::reverse(e.lhs_signals.begin(), e.lhs_signals.end()); });
  change([](auto &e) { std::reverse(e.rhs_signals.begin(), e.rhs_signals.end()); });
  change([](auto &e) { e.lhs_signals.erase(e.lhs_signals.begin()); });
  change([](auto &e) { e.lhs_signal_ids.push_back(e.lhs_signal_ids.back()); });
  change([](auto &e) { e.rhs_signal_ids.push_back(e.rhs_signal_ids.back()); });
  change([](auto &e) { e.rhs_signals.push_back(e.rhs_signals.back()); });
  change([](auto &e) { e.lhs_signal_ids.clear(); });
  change([](auto &e) { e.lhs_signals.clear(); });
  change([](auto &e) { e.rhs_signal_ids.clear(); });
  change([](auto &e) { e.rhs_signals.clear(); });
  change([](auto &e) { e.bit_map_logical_axes = e.bit_map_merged = true; });
  const auto expected = distinct;
  distinct.insert(distinct.end(), expected.begin(), expected.end());
  EndpointDedupScratch<ConstantHash> scratch;
  Require(DeduplicateEndpointsInPlace(distinct, scratch) == expected.size(), "collision/full-field removal");
  Require(distinct.size() == expected.size(), "full-field survivors lost");
  for (size_t i = 0; i < expected.size(); ++i)
    Require(SameEndpointFields(distinct[i], expected[i]), "full-field survivor mutated");

  // Equivalent path text does not make ID and string representations equal.
  auto id = Record("id"); id.lhs_signal_ids = {5}; id.lhs_signals.clear();
  auto text = id; text.lhs_signal_ids.clear(); text.lhs_signals = {"t.same"};
  auto mixed = id; mixed.lhs_signals = {"t.same"};
  auto repeated = text; repeated.lhs_signals.push_back("t.same");
  std::vector<EndpointRecord> refs{id, text, mixed, repeated, id, text};
  Require(DeduplicateEndpointsInPlace(refs, scratch) == 2 && refs.size() == 4, "ref representation collapsed");
}
}  // namespace

int main() {
  CheckStableOrder<rtl_trace::EndpointFullKeyHash>();
  CheckStableOrder<ConstantHash>();
  CheckEveryField();
  std::cout << "PASS stable full-field endpoint dedup, flags, refs, collisions, scratch reuse\n";
}
