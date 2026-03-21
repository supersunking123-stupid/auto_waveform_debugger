import argparse
import json
import subprocess


def find_child(node, name):
    for child in node.get("children", []):
        if child.get("name") == name:
            return child
    raise AssertionError(f"missing child {name!r} under {node.get('path')}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rtl-elab", required=True)
    parser.add_argument("--source", required=True)
    args = parser.parse_args()

    proc = subprocess.run(
        [args.rtl_elab, "--format", "json", "-top", "top", args.source],
        check=True,
        text=True,
        capture_output=True,
    )
    data = json.loads(proc.stdout)

    assert data["schema"] == "rtl_hier_v1"
    assert data["top"] == "top"
    assert len(data["roots"]) == 1

    root = data["roots"][0]
    assert root["kind"] == "instance"
    assert root["orig_module"] == "top"

    u0 = find_child(root, "u0")
    u1 = find_child(root, "u1")
    assert u0["orig_module"] == "child"
    assert u1["orig_module"] == "child"
    assert u0["uniq_module"] != u1["uniq_module"]

    big_scope = find_child(u0, "g_big")
    assert big_scope["kind"] == "scope"
    assert big_scope["scope_kind"] in ("generate", "generate_array")
    u_leaf = find_child(big_scope, "u_leaf")
    assert u_leaf["orig_module"] == "leaf"

    u_arr = find_child(root, "u_arr")
    assert u_arr["kind"] == "scope"
    assert u_arr["scope_kind"] == "instance_array"
    assert len(u_arr["children"]) == 2

    variants = {item["uniq_module"]: item for item in data["module_variants"]}
    assert u0["uniq_module"] in variants
    assert u1["uniq_module"] in variants

    u0_param_names = {param["name"] for param in u0["parameters"]}
    assert "WIDTH" in u0_param_names
    assert "T" in u0_param_names


if __name__ == "__main__":
    main()
