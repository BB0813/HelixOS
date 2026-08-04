#!/usr/bin/env bash
# smoke-subdir.sh — verify kernel can list /etc subdir on ESP image
# (where /etc/passwd and /etc/welcome.txt are now staged).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
if [[ -d /c/msys64/mingw64/bin ]]; then
  export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
fi

rm -f serial.log ovmf_vars.fd
# Use the helixbox ls /etc smoke path (we'll add a probe later); for now,
# run a normal smoke and grep for FAT subdir traces.
HEADLESS=1 TIMEOUT_SECS=45 bash scripts/run-qemu.sh || true
echo "=== relevant fs traces ==="
grep -a -E '\[fat\]|HelixFS|HelixFAT|load_init|loaded init' serial.log | head -20
echo "=== done ==="
