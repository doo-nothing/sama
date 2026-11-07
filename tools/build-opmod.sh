#!/usr/bin/env bash
# Build sama.opmod on this Mac with podman and the Linux aarch64 Blackfin toolchain.
#
# The Blackfin compiler (op1-engine/bfin-toolchain/bin/bfin-elf-gcc) is a Linux aarch64
# ELF and cannot run on macOS directly, so it runs inside an Ubuntu 24.04 container.
# The workspace is mounted at the same absolute path, and the kit's plugin_tool.py does
# the build and then reads the result back with `show`.
#
# First run: creates and caches a small image with python3 + python3-pyelftools
#            (this is the only step that needs network / apt).
# Later runs: no network; the build container itself runs with --network=none.
#
# iCloud: this workspace is in iCloud Drive ("Desktop & Documents"). Files that have not
# been downloaded yet are "dataless" placeholders, and podman's virtiofs mount reads
# them as EIO ("Input/output error"), which breaks compiler invocations. The script
# downloads (cat) any dataless file under the toolchain, kit or source tree first.
# Contents are not changed.
#
# Usage:
#   sama-repo/tools/build-opmod.sh
#       -> sama-repo/build/sama.opmod
#
#   SAMA_REPO=/path/to/clean/export sama-repo/tools/build-opmod.sh
#       -> /path/to/clean/export/build/sama.opmod (must be under this workspace)
#
# Environment:
#   PODMAN     podman binary (default: podman on PATH)
#   SAMA_REPO  source tree to build (default: the repo containing this script)

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # sama-repo
WS="$(cd "$HERE/.." && pwd)"                              # workspace root
KIT="$WS/op1-engine/opus-kit/plugin-kit"
TC="$WS/op1-engine/bfin-toolchain"
REPO="${SAMA_REPO:-$HERE}"
PODMAN="${PODMAN:-podman}"
IMAGE="localhost/sama-opmod-build:24.04"
OUT="$REPO/build/sama.opmod"

if [ -n "${SAMA_REPO:-}" ]; then
  REPO="$(cd "$SAMA_REPO" && pwd)"
  OUT="$REPO/build/sama.opmod"
fi

command -v "$PODMAN" >/dev/null 2>&1 || { echo "error: podman not found (use PODMAN=/opt/homebrew/bin/podman)" >&2; exit 1; }
[ -x "$TC/bin/bfin-elf-gcc" ] || { echo "error: no Blackfin toolchain at $TC" >&2; exit 1; }
[ -f "$KIT/plugin_tool.py" ] || { echo "error: no Opus kit at $KIT" >&2; exit 1; }
[ -f "$REPO/manifest.txt" ] || { echo "error: $REPO has no manifest.txt" >&2; exit 1; }

# --- iCloud: download dataless files before the container reads them ----------------
dataless="$(find "$TC" "$KIT" "$REPO" -type f ! -path '*/.git/*' \
              -exec ls -lO {} + 2>/dev/null | grep dataless | head -n 1 || true)"
if [ -n "$dataless" ]; then
  echo "Downloading iCloud placeholder files (first time can take several minutes)..."
  find "$TC" "$KIT" "$REPO" -type f ! -path '*/.git/*' \
    -exec cat {} + >/dev/null 2>&1 || true
fi

# --- cached dependency image (apt/network only the first time) ----------------------
if ! "$PODMAN" image exists "$IMAGE"; then
  echo "First run: creating $IMAGE with python3 and python3-pyelftools..."
  ctr="sama-opmod-deps-$$"
  trap '"$PODMAN" rm -f "$ctr" >/dev/null 2>&1 || true' EXIT
  "$PODMAN" run --name "$ctr" ubuntu:24.04 bash -c \
    'apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
       --no-install-recommends python3 python3-pyelftools && rm -rf /var/lib/apt/lists/*'
  "$PODMAN" commit "$ctr" "$IMAGE" >/dev/null
  "$PODMAN" rm -f "$ctr" >/dev/null
  trap - EXIT
fi

# --- build + show, with the workspace mounted at the same absolute path -------------
mkdir -p "$REPO/build"
"$PODMAN" run --rm --network=none \
  -v "$WS:$WS" \
  -e OP1_BFIN_TOOLCHAIN="$TC" \
  -e PATH="$TC/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
  "$IMAGE" bash -c '
    set -e
    python3 "$1" build "$2" --out "$3"
    python3 "$1" show "$3"
  ' bash "$KIT/plugin_tool.py" "$REPO" "$OUT"
