#!/usr/bin/env bash
# fetch-busybox.sh — download official static x86_64 musl BusyBox binary
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$ROOT/third_party/busybox"
VER="${BUSYBOX_BIN_VER:-1.35.0-x86_64-linux-musl}"
URL="https://busybox.net/downloads/binaries/${VER}/busybox"
mkdir -p "$DIR"
if [[ -f "$DIR/busybox" ]]; then
  echo "already have $DIR/busybox ($(wc -c < "$DIR/busybox") bytes)"
  exit 0
fi
echo "fetching $URL"
curl -fsSL -o "$DIR/busybox" "$URL"
chmod +x "$DIR/busybox" || true
echo "saved $DIR/busybox"
echo "Source tarball for GPL: https://busybox.net/downloads/ (match major.minor)"
ls -la "$DIR/busybox"
