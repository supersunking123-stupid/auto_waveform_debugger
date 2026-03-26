#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


UVM_INCLUDE_RE = re.compile(
    r'^\s*`include\s+"(?:uvm_pkg\.sv|uvm_macros\.svh|svt_axi\.uvm\.pkg|.*\.uvm\.(?:pkg|sv|svh))"\s*$'
)
UVM_IMPORT_RE = re.compile(
    r"^\s*import\s+(?:uvm_pkg|svt_uvm_pkg|svt_axi_uvm_pkg)(?:::\*)?;\s*$"
)
UVM_MACRO_RE = re.compile(r"^\s*`uvm_[A-Za-z0-9_]+")
PASSIVE_PKG_INCLUDE_RE = re.compile(r'^\s*`include\s+".*_passive_pkg\.sv"\s*$')
PASSIVE_PKG_IMPORT_RE = re.compile(r"^\s*import\s+.*_passive_pkg(?:::\*)?;\s*$")
DROP_LINE_TOKENS = (
    "uvm_config_db",
    "uvm_root::get",
    "run_test(",
    "type_id::create(",
    "monitor.item_observed_port.connect(",
)
SIGNAL_LOG_MACRO_RE = re.compile(r"^\s*`SVT_IF_UTIL_SUPPORT_SIGNAL_LOGGING\s*\(")
INCLUDE_RE = re.compile(r'^\s*`include\s+"([^"]+)"')


def count_word(line: str, word: str) -> int:
    return len(re.findall(rf"\b{re.escape(word)}\b", line))


def strip_protected_regions(lines: list[str]) -> list[str]:
    out: list[str] = []
    mode: str | None = None
    for line in lines:
        stripped = line.strip()
        lower = stripped.lower()
        if mode is None:
            if lower.startswith("`pragma protect begin_protected"):
                mode = "pragma"
                continue
            if lower.startswith("`protected"):
                mode = "legacy_protected"
                continue
            if lower.startswith("`protect"):
                mode = "legacy_protect"
                continue
            out.append(line)
            continue

        if mode == "pragma" and lower.startswith("`pragma protect end_protected"):
            mode = None
            continue
        if mode == "legacy_protected" and lower.startswith("`endprotected"):
            mode = None
            continue
        if mode == "legacy_protect" and lower.startswith("`endprotect"):
            mode = None
            continue
    return out


def should_drop_line(line: str) -> bool:
    stripped = line.strip()
    if not stripped:
        return False
    if UVM_INCLUDE_RE.match(stripped):
        return True
    if PASSIVE_PKG_INCLUDE_RE.match(stripped):
        return True
    if UVM_IMPORT_RE.match(stripped):
        return True
    if PASSIVE_PKG_IMPORT_RE.match(stripped):
        return True
    if UVM_MACRO_RE.match(stripped):
        return True
    if SIGNAL_LOG_MACRO_RE.match(stripped):
        return True
    if any(token in stripped for token in DROP_LINE_TOKENS):
        return True
    return False


def strip_uvm_initial_blocks(lines: list[str]) -> list[str]:
    out: list[str] = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if not re.search(r"\binitial\b", line):
            out.append(line)
            i += 1
            continue

        block = [line]
        depth = count_word(line, "begin") - count_word(line, "end")
        j = i + 1
        while depth > 0 and j < len(lines):
            block.append(lines[j])
            depth += count_word(lines[j], "begin") - count_word(lines[j], "end")
            j += 1

        block_text = "".join(block)
        block_lower = block_text.lower()
        if "uvm_config_db" in block_lower or "run_test(" in block_lower or "uvm_root::get" in block_lower:
            i = j
            continue

        out.extend(block)
        i = j
    return out


def needs_bind_shim(text: str, src: Path) -> bool:
    return (
        "svt_axi_if" in text
        and src.suffix == ".sv"
        and ("bind " in text or "module " in text)
        and '`include "svt_axi_if.svi"' not in text
    )


def bind_shim() -> str:
    return (
        "`ifndef SVT_IF_UTIL_SUPPORT_SIGNAL_LOGGING\n"
        "`define SVT_IF_UTIL_SUPPORT_SIGNAL_LOGGING(depth)\n"
        "`endif\n"
        "`ifndef SVT_AMBA_INTERFACE_METHOD_DISABLE\n"
        "`define SVT_AMBA_INTERFACE_METHOD_DISABLE\n"
        "`endif\n"
        "`ifndef CEIL\n"
        "`define CEIL(dividend, divisor) ((dividend / divisor) + ((dividend % divisor) != 0))\n"
        "`endif\n"
        '`include "svt_axi_if.svi"\n\n'
    )


def sanitize_text(text: str, src: Path) -> str:
    lines = text.splitlines(keepends=True)
    lines = strip_protected_regions(lines)
    lines = strip_uvm_initial_blocks(lines)
    lines = [line for line in lines if not should_drop_line(line)]
    sanitized = "".join(lines)
    if src.name == "nvdla_cvsram_axi_svt_bind.sv":
        sanitized = sanitized.replace(
            "  assign mon_if.master_if[0].awlen = {{($bits(mon_if.master_if[0].awlen)-4){1'b0}}, nvdla_core2cvsram_aw_awlen};\n",
            "  assign mon_if.master_if[0].awlen = nvdla_core2cvsram_aw_awlen;\n",
        )
        sanitized = sanitized.replace(
            "  assign mon_if.master_if[0].arlen = {{($bits(mon_if.master_if[0].arlen)-4){1'b0}}, nvdla_core2cvsram_ar_arlen};\n",
            "  assign mon_if.master_if[0].arlen = nvdla_core2cvsram_ar_arlen;\n",
        )
    if needs_bind_shim(sanitized, src):
        sanitized = bind_shim() + sanitized
    return sanitized


def extract_includes(text: str) -> list[str]:
    includes: list[str] = []
    for line in text.splitlines():
        match = INCLUDE_RE.match(line)
        if match:
            includes.append(match.group(1))
    return includes


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate rtl_trace-friendly SVT AXI interface / bind sources."
    )
    parser.add_argument(
        "--input",
        action="append",
        required=True,
        help="Input file to sanitize. Can be specified multiple times.",
    )
    parser.add_argument("--outdir", required=True, help="Output directory for sanitized files.")
    parser.add_argument(
        "--incdir",
        action="append",
        default=[],
        help="Additional include directory to search recursively.",
    )
    parser.add_argument(
        "--keep-names",
        action="store_true",
        help="Preserve the original basenames in the output directory.",
    )
    return parser.parse_args()


def find_include(name: str, search_dirs: list[Path]) -> Path | None:
    for directory in search_dirs:
        candidate = directory / name
        if candidate.is_file():
            return candidate.resolve()
    return None


def process_file(
    src: Path,
    outdir: Path,
    search_dirs: list[Path],
    visited: set[Path],
) -> None:
    src = src.resolve()
    if src in visited:
        return
    visited.add(src)

    sanitized = sanitize_text(src.read_text(encoding="utf-8"), src)
    dst = outdir / src.name
    dst.write_text(sanitized, encoding="utf-8")
    print(dst)

    nested_dirs = [src.parent, *search_dirs]
    for include_name in extract_includes(sanitized):
        nested = find_include(include_name, nested_dirs)
        if nested is not None:
            process_file(nested, outdir, search_dirs, visited)


def main() -> int:
    args = parse_args()
    outdir = Path(args.outdir).expanduser().resolve()
    outdir.mkdir(parents=True, exist_ok=True)
    search_dirs = [Path(p).expanduser().resolve() for p in args.incdir]
    visited: set[Path] = set()

    for raw in args.input:
        src = Path(raw).expanduser().resolve()
        if not src.is_file():
            print(f"missing input: {src}", file=sys.stderr)
            return 1
        process_file(src, outdir, [src.parent, *search_dirs], visited)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
