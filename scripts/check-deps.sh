#!/usr/bin/env bash
# check-deps.sh — verify HelixOS M0 toolchain
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Prefer MSYS2 MinGW64 if present (Windows dev path)
if [[ -d /c/msys64/mingw64/bin ]]; then
  export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
elif [[ -d /mingw64/bin ]]; then
  export PATH="/mingw64/bin:/usr/bin:$PATH"
fi

ok=0
bad=0

have() {
  local name="$1"
  shift
  if command -v "$name" >/dev/null 2>&1; then
    echo "  OK  $name  ($(command -v "$name"))"
    if [[ $# -gt 0 ]]; then
      "$@" 2>/dev/null | head -1 | sed 's/^/      /' || true
    fi
    ok=$((ok + 1))
    return 0
  else
    echo "  MISSING  $name"
    bad=$((bad + 1))
    return 1
  fi
}

echo "== HelixOS dependency check =="
echo "ROOT=$ROOT"
echo

echo "[compilers]"
have clang clang --version || true
have lld-link lld-link --version || true
# fallback name on some Linux distros
if ! command -v lld-link >/dev/null 2>&1; then
  if command -v lld >/dev/null 2>&1; then
    echo "  note: 'lld' present but 'lld-link' missing — install lld package that provides lld-link"
  fi
fi
have make make --version || true

echo
echo "[runtime]"
have qemu-system-x86_64 qemu-system-x86_64 --version || true

echo
echo "[OVMF / EDK2 firmware]"
FOUND_OVMF=""
CANDIDATES=(
  "${OVMF_CODE:-}"
  "/mingw64/share/qemu/edk2-x86_64-code.fd"
  "/c/msys64/mingw64/share/qemu/edk2-x86_64-code.fd"
  "/usr/share/OVMF/OVMF_CODE.fd"
  "/usr/share/OVMF/OVMF_CODE_4M.fd"
  "/usr/share/edk2/ovmf/OVMF_CODE.fd"
  "/usr/share/edk2-ovmf/x64/OVMF_CODE.fd"
  "/usr/share/qemu/OVMF.fd"
  "/usr/share/ovmf/OVMF.fd"
)
for f in "${CANDIDATES[@]}"; do
  [[ -z "$f" ]] && continue
  if [[ -f "$f" ]]; then
    echo "  OK  OVMF_CODE = $f"
    FOUND_OVMF="$f"
    ok=$((ok + 1))
    break
  fi
done
if [[ -z "$FOUND_OVMF" ]]; then
  echo "  MISSING  OVMF code firmware (set OVMF_CODE=...)"
  bad=$((bad + 1))
fi

echo
if [[ "$bad" -eq 0 ]]; then
  echo "All required tools found ($ok checks)."
  exit 0
else
  echo "Missing $bad required item(s). See docs/BUILD.md"
  exit 1
fi
