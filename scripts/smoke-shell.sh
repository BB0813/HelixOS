#!/usr/bin/env bash
# smoke-shell.sh — headless QEMU with TCP serial; feed help/mem/page/int
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SERIAL_LOG="${SERIAL_LOG:-$ROOT/serial.log}"
ESP_IMG="${ESP_IMG:-$ROOT/out/esp.img}"
PORT="${HELIX_SERIAL_PORT:-4659}"

if [[ -d /c/msys64/mingw64/bin ]]; then
  export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
elif [[ -d /mingw64/bin ]]; then
  export PATH="/mingw64/bin:/usr/bin:$PATH"
fi

export TMPDIR="${TMPDIR:-/c/Users/hbq30/AppData/Local/Temp}"
export TEMP="${TEMP:-C:/Users/hbq30/AppData/Local/Temp}"
export TMP="${TMP:-$TEMP}"

if [[ ! -f "$ROOT/esp/EFI/BOOT/BOOTX64.EFI" ]]; then
  echo "error: run 'make esp' first" >&2
  exit 1
fi

# Rebuild disk image (do this BEFORE disabling MSYS path conversion —
# native python.exe needs a Windows path for mkdisk.py).
bash "$ROOT/scripts/mkesp.sh"

# Native qemu.exe: stop MSYS from rewriting -drive file=... arguments.
export MSYS2_ARG_CONV_EXCL='*'

to_win_path() {
  local p="$1"
  if command -v cygpath >/dev/null 2>&1; then
    cygpath -w "$p" | sed 's|\\|/|g'
    return
  fi
  if [[ "$p" =~ ^/([a-zA-Z])/(.*)$ ]]; then
    echo "${BASH_REMATCH[1]^^}:/${BASH_REMATCH[2]}"
    return
  fi
  echo "$p"
}

OVMF_CODE=""
for c in \
  "/c/msys64/mingw64/share/qemu/edk2-x86_64-code.fd" \
  "/mingw64/share/qemu/edk2-x86_64-code.fd" \
  "/usr/share/OVMF/OVMF_CODE.fd" \
  "/usr/share/OVMF/OVMF_CODE_4M.fd" \
  "/usr/share/edk2/ovmf/OVMF_CODE.fd"
do
  [[ -f "$c" ]] && OVMF_CODE="$c" && break
done
[[ -n "$OVMF_CODE" ]] || { echo "OVMF not found" >&2; exit 1; }

VARS_SRC=""
for c in \
  "/c/msys64/mingw64/share/qemu/edk2-i386-vars.fd" \
  "/mingw64/share/qemu/edk2-i386-vars.fd" \
  "/usr/share/OVMF/OVMF_VARS.fd"
do
  [[ -f "$c" ]] && VARS_SRC="$c" && break
done

VARS_DST="$ROOT/ovmf_vars.fd"
if [[ -n "$VARS_SRC" ]]; then
  cp "$VARS_SRC" "$VARS_DST"
else
  dd if=/dev/zero of="$VARS_DST" bs=1048576 count=2 2>/dev/null
fi

CODE_ARG="$(to_win_path "$OVMF_CODE")"
VARS_ARG="$(to_win_path "$VARS_DST")"
DISK_ARG="$(to_win_path "$ESP_IMG")"
LOG_ARG="$(to_win_path "$SERIAL_LOG")"

rm -f "$SERIAL_LOG"

echo "smoke-shell: TCP serial 127.0.0.1:$PORT  log=$SERIAL_LOG"

# Start QEMU in background; serial is a TCP server (nowait).
qemu-system-x86_64 \
  -machine q35,accel=tcg \
  -cpu qemu64 \
  -m 256M \
  -drive if=pflash,format=raw,readonly=on,file="$CODE_ARG" \
  -drive if=pflash,format=raw,file="$VARS_ARG" \
  -drive if=none,id=helixesp,format=raw,file="$DISK_ARG" \
  -device ich9-ahci,id=ahci \
  -device ide-hd,drive=helixesp,bus=ahci.0,bootindex=1 \
  -net none \
  -chardev socket,id=com1,host=127.0.0.1,port="$PORT",server=on,wait=off,logfile="$LOG_ARG" \
  -serial chardev:com1 \
  -display none \
  -name "HelixOS-smoke-shell" \
  &
QEMU_PID=$!

cleanup() {
  kill "$QEMU_PID" 2>/dev/null || true
  wait "$QEMU_PID" 2>/dev/null || true
}
trap cleanup EXIT

# Wait until log shows shell ready (or timeout).
ready=0
for i in $(seq 1 40); do
  if grep -a -q "M2 shell ready" "$SERIAL_LOG" 2>/dev/null; then
    ready=1
    break
  fi
  if ! kill -0 "$QEMU_PID" 2>/dev/null; then
    echo "qemu exited early" >&2
    break
  fi
  sleep 0.5
done

if [[ "$ready" != "1" ]]; then
  echo "SMOKE-SHELL FAIL — shell never became ready"
  tail -40 "$SERIAL_LOG" 2>/dev/null || true
  exit 1
fi

# Small settle delay after prompt
sleep 1

PY="$(command -v python3 || command -v python || true)"
if [[ -z "$PY" ]]; then
  echo "python required to feed TCP serial" >&2
  exit 1
fi

"$PY" - "$PORT" <<'PY'
import socket, sys, time
port = int(sys.argv[1])
cmds = [b"help\r", b"mem\r", b"page\r", b"int\r", b"uptime\r"]
# retry connect a few times
s = None
for _ in range(20):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=2)
        break
    except OSError:
        time.sleep(0.25)
if s is None:
    sys.stderr.write("cannot connect to serial TCP\n")
    sys.exit(2)
s.settimeout(2)
for c in cmds:
    s.sendall(c)
    time.sleep(0.4)
time.sleep(1.5)
try:
    s.shutdown(socket.SHUT_WR)
except OSError:
    pass
s.close()
PY

# Give kernel a moment to finish printing
sleep 1

echo "==== smoke-shell: serial excerpt ===="
grep -a -E 'helix>|Helix kernel shell|PMM:|Paging:|timer:|IRQ|uptime:|M2 shell|\[tick\]' \
  "$SERIAL_LOG" 2>/dev/null | head -80 || true

ok=1
for pat in \
  "Helix kernel shell" \
  "PMM: total_pages" \
  "Paging: identity map" \
  "timer: ticks=" \
  "uptime:"
do
  if ! grep -a -q "$pat" "$SERIAL_LOG" 2>/dev/null; then
    echo "SMOKE-SHELL FAIL — missing '$pat'"
    ok=0
  fi
done

if [[ "$ok" = "1" ]]; then
  echo "SMOKE-SHELL OK"
  exit 0
fi

echo "---- serial.log (tail) ----"
tail -100 "$SERIAL_LOG" 2>/dev/null || echo "(empty)"
exit 1
