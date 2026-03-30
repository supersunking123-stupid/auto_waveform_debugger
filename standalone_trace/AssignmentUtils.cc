#include "AssignmentUtils.h"

#include <cctype>

namespace {

size_t FindAssignmentOperator(std::string_view text, size_t *operator_width) {
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '<' && i + 1 < text.size() && text[i + 1] == '=') {
      *operator_width = 2;
      return i;
    }
    if (text[i] != '=') continue;

    const char prev = (i > 0) ? text[i - 1] : '\0';
    const char next = (i + 1 < text.size()) ? text[i + 1] : '\0';
    if (prev == '<' || prev == '>' || prev == '=' || prev == '!') continue;
    if (next == '=') continue;
    *operator_width = 1;
    return i;
  }
  return std::string_view::npos;
}

} // namespace

std::string TrimWhitespace(std::string_view text) {
  size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  return std::string(text.substr(begin, end - begin));
}

std::vector<std::string> InferAssignmentLhsPathsFromText(const std::string &endpoint_path,
                                                         std::string_view assignment_text) {
  if (assignment_text.empty()) return {};

  size_t operator_width = 0;
  const size_t assignment_op = FindAssignmentOperator(assignment_text, &operator_width);
  if (assignment_op == std::string::npos || operator_width == 0) return {};

  std::string lhs = TrimWhitespace(assignment_text.substr(0, assignment_op));
  if (lhs.rfind("assign ", 0) == 0) lhs = TrimWhitespace(std::string_view(lhs).substr(7));
  if (lhs.empty()) return {};

  if (lhs.rfind("top.", 0) == 0) return {lhs};

  const size_t dot = endpoint_path.rfind('.');
  if (dot == std::string::npos) return {};
  return {endpoint_path.substr(0, dot) + "." + lhs};
}
