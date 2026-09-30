#pragma once

#include <span>
#include <string>
#include <string_view>
#include <utility>

// Internal canonical-frame path translation. Interface leaf roots include array indices.
inline bool UnderCanonicalRoot(std::string_view path, std::string_view root) {
  return path.size() >= root.size() && path.compare(0, root.size(), root) == 0 &&
         (path.size() == root.size() || path[root.size()] == '.');
}

// Roots must be sorted longest-first. A frame applies exactly one substitution;
// identity entries therefore protect interface roots from module-root rewriting.
inline bool ApplyCanonicalFramePath(
    std::string &path, std::string_view src, const std::string &dst,
    std::span<const std::pair<std::string, std::string>> roots, bool &external_iface) {
  for (const auto &root : roots) {
    if (!UnderCanonicalRoot(path, root.first)) continue;
    path = root.second + path.substr(root.first.size());
    external_iface = true;
    return true;
  }
  if (!UnderCanonicalRoot(path, src)) return false;
  path = dst + path.substr(src.size());
  return true;
}
