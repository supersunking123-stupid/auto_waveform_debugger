#include "db/CanonicalPath.h"

#include <iostream>
#include <vector>

using Roots = std::vector<std::pair<std::string, std::string>>;

int main() {
  int failures = 0;
  auto check = [&](bool good, const char *case_name) {
    if (!good) { std::cerr << "FAIL: " << case_name << '\n'; ++failures; }
  };
  auto translate = [&](std::string path, const Roots &roots, std::string_view expected,
                       bool expect_external, const char *name) {
    bool external = false;
    check(ApplyCanonicalFramePath(path, "top.rep", "top.actual", roots, external), name);
    check(path == expected && external == expect_external, name);
  };
  translate("top.rep.data", {}, "top.actual.data", false, "module path");
  translate("top.bus[1].data", {{"top.bus[1]", "top.bus[9]"}},
            "top.bus[9].data", true, "interface array leaf");
  translate("top.rep.bus.data", {{"top.rep.bus", "top.rep.bus"}},
            "top.rep.bus.data", true, "identity overrides module prefix");
  translate("top.rep.bus.inner.data", {{"top.rep.bus.inner", "top.deep"},
                                      {"top.rep.bus", "top.shallow"}},
            "top.deep.data", true, "longest root wins");
  translate("top.a.data", {{"top.a", "top.b"}, {"top.b", "top.c"}},
            "top.b.data", true, "one rewrite per frame");
  check(UnderCanonicalRoot("top.bus", "top.bus"), "exact root");
  check(!UnderCanonicalRoot("top.bus2.data", "top.bus"), "name boundary");
  check(!UnderCanonicalRoot("top.bus[10].data", "top.bus[1]"), "array index boundary");
  check(!UnderCanonicalRoot("top.replica.data", "top.rep"), "module boundary");
  {
    bool external = false;
    std::string path = "top.iface.data";
    Roots inner{{"top.iface", "top.rep.local"}};
    check(ApplyCanonicalFramePath(path, "top.child0", "top.rep.child1", inner, external), "inner frame");
    check(ApplyCanonicalFramePath(path, "top.rep", "top.actual", {}, external), "outer module frame");
    check(path == "top.actual.local.data" && external, "local interface follows enclosing module");
  }
  {
    bool external = false;
    std::string path = "top.a.data";
    Roots inner{{"top.a", "top.b"}}, outer{{"top.b", "top.c"}};
    ApplyCanonicalFramePath(path, "top.rep", "top.actual", inner, external);
    ApplyCanonicalFramePath(path, "top.other", "top.copy", outer, external);
    check(path == "top.c.data" && external, "continue interface rewrite in outer frame");
    check(!ApplyCanonicalFramePath(path, "top.unrelated", "top.copy", {}, external), "external root untouched");
    check(path == "top.c.data" && external, "external validity persists");
  }
  return failures ? 1 : 0;
}
