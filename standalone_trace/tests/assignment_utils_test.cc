#include "AssignmentUtils.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

bool ExpectEq(const std::vector<std::string> &got, const std::vector<std::string> &want,
              const char *label) {
  if (got == want) return true;
  std::cerr << label << " mismatch\n";
  std::cerr << "  got:";
  for (const auto &item : got) std::cerr << " [" << item << "]";
  std::cerr << "\n  want:";
  for (const auto &item : want) std::cerr << " [" << item << "]";
  std::cerr << "\n";
  return false;
}

} // namespace

int main() {
  if (!ExpectEq(
          InferAssignmentLhsPathsFromText("semantic_top.hit", "flag <= hit"),
          {"semantic_top.flag"},
          "nonblocking")) {
    return EXIT_FAILURE;
  }

  if (!ExpectEq(
          InferAssignmentLhsPathsFromText("semantic_top.out", "assign out = (a == b)"),
          {"semantic_top.out"},
          "blocking_with_comparison_rhs")) {
    return EXIT_FAILURE;
  }

  // Test 3 — Empty assignment text
  if (!ExpectEq(
          InferAssignmentLhsPathsFromText("", ""),
          {},
          "empty_assignment_text")) {
    return EXIT_FAILURE;
  }

  // Test 4 — LHS starting with "top."
  if (!ExpectEq(
          InferAssignmentLhsPathsFromText("x", "top.flag <= x"),
          {"top.flag"},
          "lhs_starting_with_top_dot")) {
    return EXIT_FAILURE;
  }

  // Test 5 — Blocking assignment with ternary and ==
  if (!ExpectEq(
          InferAssignmentLhsPathsFromText("top.out", "assign out = (a == b) ? c : d"),
          {"top.out"},
          "blocking_with_ternary_and_eq")) {
    return EXIT_FAILURE;
  }

  std::cout << "assignment_utils_test: PASS\n";
  return EXIT_SUCCESS;
}
