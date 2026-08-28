#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Build and run everything that can be checked automatically.
#
#   scripts/run_tests.sh              # native suites only
#   scripts/run_tests.sh --all        # plus the integration suites
#
# The integration suites need the upstream server package installed
# (pip install -e <STYLY-NetSync>/STYLY-NetSync-Server) and, for the Godot one,
# a Godot 4 binary in $GODOT. Both are skipped with a note when unavailable.
set -euo pipefail

cd "$(dirname "$0")/.."
BUILD_DIR="${BUILD_DIR:-build}"
RUN_INTEGRATION=0
[[ "${1:-}" == "--all" ]] && RUN_INTEGRATION=1

if [[ ! -f third_party/libzmq/CMakeLists.txt ]]; then
  echo "third_party/libzmq is empty. Run: git submodule update --init --recursive" >&2
  exit 1
fi

echo "=== Building native tests ==="
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release ${NINJA:+-G Ninja} >/dev/null
cmake --build "$BUILD_DIR" --parallel

echo
echo "=== Native test suites ==="
ctest --test-dir "$BUILD_DIR" --output-on-failure

if [[ "$RUN_INTEGRATION" -eq 0 ]]; then
  echo
  echo "Integration suites skipped. Re-run with --all to include them."
  exit 0
fi

if ! python3 -c "import styly_netsync" 2>/dev/null; then
  echo
  echo "SKIP integration: the upstream server package is not installed." >&2
  echo "  pip install -e <STYLY-NetSync checkout>/STYLY-NetSync-Server" >&2
  exit 0
fi

echo
echo "=== Native client against a real server ==="
python3 tests/integration/test_against_server.py --probe "$BUILD_DIR/netsync_probe"

GODOT_BIN="${GODOT:-$(command -v godot || true)}"
if [[ -z "$GODOT_BIN" ]]; then
  echo
  echo "SKIP the Godot suite: no Godot binary. Set \$GODOT or put godot on PATH." >&2
  exit 0
fi
if ! compgen -G "addons/styly_netsync/bin/libstyly_netsync.*" >/dev/null; then
  echo
  echo "SKIP the Godot suite: the extension is not built (scons target=template_debug)." >&2
  exit 0
fi

echo
echo "=== Godot addon against a real server ==="
# Godot only discovers a .gdextension during the scan that first sees it, so the
# first import cannot compile the scripts that use it.
"$GODOT_BIN" --headless --path . --editor --quit >/dev/null 2>&1 || true
"$GODOT_BIN" --headless --path . --editor --quit >/dev/null 2>&1
GODOT="$GODOT_BIN" python3 tests/integration/test_godot_client.py
