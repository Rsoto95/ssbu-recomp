#!/usr/bin/env bash
# Starts the native Linux build (BUILDING_LINUX.md). Run it from anywhere:
#
#   ./play.sh                          play; saves in ~/.local/share/ssbu-recomp/userdata
#   ./play.sh --userdata <folder>      another save folder
#   ./play.sh --help                   every option (they are scripts/run.py's)
#
# The launcher's output is shown here and kept in ~/.local/share/ssbu-recomp/last_run.log; the game's own log goes to
# build-linux/logs/game_*.log (the last 10), and is sent to the group's server after each online match (share_logs = off: not).
set -uo pipefail

REPO="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}/ssbu-recomp"
PY="$REPO/deps/venv/bin/python"

[ -x "$PY" ] || { echo "deps/venv is missing: run scripts/linux/install_deps.sh first" >&2; exit 1; }
# run.py checks the selected build, including --build, and can repair a missing runtime.
# Parse the complete block before Python starts: git pull may replace this launcher while it waits.
{
    mkdir -p "$DATA"
    "$PY" "$REPO/scripts/run.py" --linux --play "$@" 2>&1 | tee "$DATA/last_run.log"
    exit "${PIPESTATUS[0]}"
}
