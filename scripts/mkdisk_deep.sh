#!/usr/bin/env bash
# mkdisk_deep.sh — build a 64 MiB FAT32 image with a 4-level nested directory
# tree (a/b/c/d/file.txt) and verify with mtools. Used for M20 deep subdir test.
#
# Output: out/helix-deep.img + mtools verification.
#
# Does NOT replace the main ESP image (out/esp.img).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${DEEP_IMG:-$ROOT/out/helix-deep.img}"
TMPTREE="${DEEP_TREE:-$ROOT/out/deep-tree}"

if [[ -d /c/msys64/mingw64/bin ]]; then
  export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
elif [[ -d /mingw64/bin ]]; then
  export PATH="/mingw64/bin:/usr/bin:$PATH"
fi

PY="$(command -v python3 || command -v python || true)"
if [[ -z "$PY" ]]; then
  echo "error: python required" >&2
  exit 1
fi

rm -rf "$TMPTREE"
mkdir -p "$TMPTREE/a/b/c/d"
printf "HELIX_DEEP_OK\n" > "$TMPTREE/a/b/c/d/file.txt"
# Add a second file at each level to exercise multi-entry iteration
printf "level1\n" > "$TMPTREE/a/level1.txt"
printf "level2\n" > "$TMPTREE/a/b/level2.txt"
printf "level3\n" > "$TMPTREE/a/b/c/level3.txt"

mkdir -p "$(dirname "$IMG")"

# mkdisk.py is run by native Windows python.exe — it needs Windows paths.
case "$(uname -s)" in
  MINGW*|MSYS*) WIN_TMPTREE="$(cygpath -w "$TMPTREE")" ;;
  *)            WIN_TMPTREE="$TMPTREE" ;;
esac

# Produce a raw FAT volume (no GPT) so mtools can read it directly.
RAW_IMG="${IMG%.img}.raw.img"
"$PY" "$ROOT/scripts/mkdisk.py" \
  --efi "$ROOT/out/BOOTX64.EFI" \
  --out "$RAW_IMG" \
  --esp-mib 64 \
  --raw-fat \
  --add-tree "${WIN_TMPTREE}::"

echo ""
echo "=== mtools verification ==="
# Copy from MSYS path to Windows path for mtools
case "$(uname -s)" in
  MINGW*|MSYS*) HOST_IMG="$(cygpath -w "$IMG")" ;;
  *)            HOST_IMG="$IMG" ;;
esac

case "$(uname -s)" in
  MINGW*|MSYS*) MT_IMG="$(cygpath -w "$RAW_IMG")" ;;
  *)            MT_IMG="$RAW_IMG" ;;
esac

if ! command -v mdir >/dev/null; then
  echo "SKIP: mtools not installed"
  echo "deep img (raw FAT): $RAW_IMG"
  exit 0
fi

echo "[::a]"
mdir -/ -i "$MT_IMG" ::a
echo "[::a/b]"
mdir -/ -i "$MT_IMG" ::a/b
echo "[::a/b/c]"
mdir -/ -i "$MT_IMG" ::a/b/c
echo "[::a/b/c/d]"
mdir -/ -i "$MT_IMG" ::a/b/c/d
echo "[::a/b/c/d/file.txt]"
mtype -i "$MT_IMG" ::a/b/c/d/file.txt

echo ""
echo "DEEP-IMG OK: $IMG"
