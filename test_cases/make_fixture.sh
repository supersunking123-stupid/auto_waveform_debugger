#!/bin/bash
# Regenerate the cross-link test fixture (used by tc16-tc27 and the
# NVDLA-free integration tests in agent_debug_automation/tests):
#
#   test_cases/rtl_trace.db  - structural DB built with standalone_trace/build/rtl_trace
#   test_cases/wave.fsdb     - FSDB dumped by VCS + Verdi PLI from the same design
#
# Design/testbench sources live in test_cases/cross_link_fixture/src (top
# module `top`). Build products are kept in test_cases/cross_link_fixture/build.
#
# Usage:
#   ./make_fixture.sh            # build only if outputs are missing or stale
#   ./make_fixture.sh --force    # always rebuild
#   ./make_fixture.sh --check    # exit 0 if outputs are present and up to date
#
# Overridable environment: RTL_TRACE, VCS_BIN, VCS_HOME, VERDI_HOME.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
FIXTURE_DIR="$SCRIPT_DIR/cross_link_fixture"
BUILD_DIR="$FIXTURE_DIR/build"
DB_OUT="$SCRIPT_DIR/rtl_trace.db"
FSDB_OUT="$SCRIPT_DIR/wave.fsdb"

RTL_TRACE="${RTL_TRACE:-$ROOT_DIR/standalone_trace/build/rtl_trace}"
VCS_BIN="${VCS_BIN:-$(command -v vcs || echo /home/qsun/eda_tools/vcs/W-2024.09-SP1/bin/vcs)}"
export VCS_HOME="${VCS_HOME:-$(dirname "$(dirname "$VCS_BIN")")}"
export VERDI_HOME="${VERDI_HOME:-/home/qsun/eda_tools/verdi/W-2024.09-SP1}"

MODE="auto"
case "${1:-}" in
    --force) MODE="force" ;;
    --check) MODE="check" ;;
    "") ;;
    *) echo "Usage: $0 [--force|--check]" >&2; exit 2 ;;
esac

is_up_to_date() {
    [ -s "$DB_OUT" ] && [ -s "$FSDB_OUT" ] || return 1
    local newer
    newer="$(find "$FIXTURE_DIR/src" "$FIXTURE_DIR/files.f" -newer "$FSDB_OUT" -print -quit)"
    [ -z "$newer" ] || return 1
    newer="$(find "$FIXTURE_DIR/src" "$FIXTURE_DIR/files.f" -newer "$DB_OUT" -print -quit)"
    [ -z "$newer" ]
}

if [ "$MODE" = "check" ]; then
    is_up_to_date
    exit $?
fi
if [ "$MODE" = "auto" ] && is_up_to_date; then
    echo "[make_fixture] fixture up to date: $DB_OUT, $FSDB_OUT"
    exit 0
fi

[ -x "$RTL_TRACE" ] || { echo "[make_fixture] ERROR: rtl_trace binary not found: $RTL_TRACE (build standalone_trace first)" >&2; exit 1; }
[ -x "$VCS_BIN" ] || { echo "[make_fixture] ERROR: vcs not found ($VCS_BIN); set VCS_BIN/VCS_HOME/VERDI_HOME" >&2; exit 1; }
[ -d "$VERDI_HOME" ] || { echo "[make_fixture] ERROR: VERDI_HOME not found: $VERDI_HOME" >&2; exit 1; }

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

# Files are listed relative to the fixture dir in files.f.
cd "$FIXTURE_DIR"

echo "[make_fixture] Step 1: rtl_trace compile -> rtl_trace.db"
"$RTL_TRACE" compile --db "$BUILD_DIR/rtl_trace.db" --top top -f files.f > "$BUILD_DIR/rtl_trace_compile.log" 2>&1 \
    || { cat "$BUILD_DIR/rtl_trace_compile.log" >&2; echo "[make_fixture] ERROR: rtl_trace compile failed" >&2; exit 1; }

echo "[make_fixture] Step 2: VCS compile (+FSDB dump)"
cd "$BUILD_DIR"
# files.f is relative to the fixture dir; give VCS an absolute list.
sed "s|^|$FIXTURE_DIR/|" "$FIXTURE_DIR/files.f" > files.abs.f
"$VCS_BIN" -full64 -sverilog -top top -file files.abs.f +define+FSDB \
    -debug_access+all -kdb -o simv -l vcs.log > vcs.stdout 2>&1 \
    || { tail -40 vcs.log >&2; echo "[make_fixture] ERROR: VCS compile failed (see $BUILD_DIR/vcs.log)" >&2; exit 1; }

echo "[make_fixture] Step 3: simulate -> wave.fsdb"
./simv -l sim.log > sim.stdout 2>&1 \
    || { tail -40 sim.log >&2; echo "[make_fixture] ERROR: simulation failed (see $BUILD_DIR/sim.log)" >&2; exit 1; }
[ -s wave.fsdb ] || { echo "[make_fixture] ERROR: simulation produced no wave.fsdb" >&2; exit 1; }

# Publish outputs only after everything succeeded.
mv -f rtl_trace.db "$DB_OUT"
[ -f rtl_trace.db.meta ] && mv -f rtl_trace.db.meta "$DB_OUT.meta"
mv -f wave.fsdb "$FSDB_OUT"
echo "[make_fixture] done: $DB_OUT, $FSDB_OUT"
