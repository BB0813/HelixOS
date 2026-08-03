#!/usr/bin/env bash
# run-qemu.sh — boot HelixOS ESP under QEMU + OVMF (UEFI)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ESP="${ESP:-$ROOT/esp}"
ESP_IMG="${ESP_IMG:-$ROOT/out/esp.img}"
SERIAL_LOG="${SERIAL_LOG:-$ROOT/serial.log}"

if [[ -d /c/msys64/mingw64/bin ]]; then
  export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
elif [[ -d /mingw64/bin ]]; then
  export PATH="/mingw64/bin:/usr/bin:$PATH"
fi

if [[ ! -f "$ESP/EFI/BOOT/BOOTX64.EFI" ]]; then
  echo "error: $ESP/EFI/BOOT/BOOTX64.EFI missing — run 'make esp' first" >&2
  exit 1
fi

# Always (re)build the FAT image — fat:rw:dir is broken on some Windows/QEMU builds
# ("Could not open temporary file C:\/vl.XXXX").
bash "$ROOT/scripts/mkesp.sh"

to_win_path() {
  local p="$1"
  if command -v cygpath >/dev/null 2>&1; then
    cygpath -w "$p" | sed 's|\\|/|g'
    return
  fi
  if [[ "$p" =~ ^/([a-zA-Z])/(.*)$ ]]; then
    local drive="${BASH_REMATCH[1]}"
    local rest="${BASH_REMATCH[2]}"
    echo "${drive^^}:/$rest"
    return
  fi
  echo "$p"
}

QEMU_BIN="$(command -v qemu-system-x86_64)"
NEED_WIN_PATHS=0
case "$QEMU_BIN" in
  *.exe|*/mingw64/*|*/msys64/*) NEED_WIN_PATHS=1 ;;
esac
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) NEED_WIN_PATHS=1 ;;
esac

find_ovmf_code() {
  local c
  for c in \
    "${OVMF_CODE:-}" \
    "$ROOT/third_party/ovmf-ubuntu/usr/share/OVMF/OVMF_CODE_4M.fd" \
    "$ROOT/third_party/ovmf/usr/share/edk2/x64/OVMF_CODE.4m.fd" \
    "/mingw64/share/qemu/edk2-x86_64-code.fd" \
    "/c/msys64/mingw64/share/qemu/edk2-x86_64-code.fd" \
    "/usr/share/OVMF/OVMF_CODE.fd" \
    "/usr/share/OVMF/OVMF_CODE_4M.fd" \
    "/usr/share/edk2/ovmf/OVMF_CODE.fd" \
    "/usr/share/edk2-ovmf/x64/OVMF_CODE.fd" \
    "/usr/share/qemu/OVMF.fd" \
    "/usr/share/ovmf/OVMF.fd"
  do
    [[ -z "$c" ]] && continue
    if [[ -f "$c" ]]; then
      if [[ "$c" == /mingw64/* && -f "/c/msys64$c" ]]; then
        echo "/c/msys64$c"
      else
        echo "$c"
      fi
      return 0
    fi
  done
  return 1
}

find_ovmf_vars() {
  local c
  for c in \
    "${OVMF_VARS:-}" \
    "$ROOT/third_party/ovmf-ubuntu/usr/share/OVMF/OVMF_VARS_4M.fd" \
    "$ROOT/third_party/ovmf/usr/share/edk2/x64/OVMF_VARS.4m.fd" \
    "/mingw64/share/qemu/edk2-i386-vars.fd" \
    "/c/msys64/mingw64/share/qemu/edk2-i386-vars.fd" \
    "/usr/share/OVMF/OVMF_VARS.fd" \
    "/usr/share/OVMF/OVMF_VARS_4M.fd" \
    "/usr/share/edk2/ovmf/OVMF_VARS.fd" \
    "/usr/share/edk2-ovmf/x64/OVMF_VARS.fd"
  do
    [[ -z "$c" ]] && continue
    if [[ -f "$c" ]]; then
      if [[ "$c" == /mingw64/* && -f "/c/msys64$c" ]]; then
        echo "/c/msys64$c"
      else
        echo "$c"
      fi
      return 0
    fi
  done
  return 1
}

OVMF_CODE_FILE="$(find_ovmf_code)" || {
  echo "error: OVMF code firmware not found; set OVMF_CODE=..." >&2
  exit 1
}

VARS_SRC=""
if VARS_SRC="$(find_ovmf_vars)"; then
  :
else
  VARS_SRC=""
fi

VARS_DST="$ROOT/ovmf_vars.fd"
if [[ -n "$VARS_SRC" ]]; then
  cp "$VARS_SRC" "$VARS_DST"
else
  if command -v qemu-img >/dev/null 2>&1; then
    qemu-img create -f raw "$VARS_DST" 2M >/dev/null
  else
    dd if=/dev/zero of="$VARS_DST" bs=1048576 count=2 2>/dev/null
  fi
fi

CODE_ARG="$OVMF_CODE_FILE"
VARS_ARG="$VARS_DST"
DISK_ARG="$ESP_IMG"
LOG_ARG="$SERIAL_LOG"

if [[ "$NEED_WIN_PATHS" -eq 1 ]]; then
  export MSYS2_ARG_CONV_EXCL='*'
  CODE_ARG="$(to_win_path "$OVMF_CODE_FILE")"
  VARS_ARG="$(to_win_path "$VARS_DST")"
  DISK_ARG="$(to_win_path "$ESP_IMG")"
  LOG_ARG="$(to_win_path "$SERIAL_LOG")"
fi

echo "OVMF_CODE = $CODE_ARG"
echo "OVMF_VARS = $VARS_ARG (from ${VARS_SRC:-empty})"
echo "ESP_IMG   = $DISK_ARG"
echo "serial    → stdio + $LOG_ARG"
echo

QEMU_DISPLAY_ARGS=(-display gtk -device VGA)
if [[ "${HEADLESS:-0}" == "1" ]] || [[ "${DISPLAY_MODE:-}" == "nographic" ]]; then
  QEMU_DISPLAY_ARGS=(-display none)
fi

TIMEOUT_CMD=()
if [[ -n "${TIMEOUT_SECS:-}" ]]; then
  if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_CMD=(timeout --foreground "${TIMEOUT_SECS}")
  elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_CMD=(gtimeout --foreground "${TIMEOUT_SECS}")
  fi
fi

# Fresh OVMF vars each smoke run avoid sticky boot-order junk.
# IDE on q35: use ahci + bootindex so OVMF tries our GPT ESP first.
exec "${TIMEOUT_CMD[@]}" qemu-system-x86_64 \
  -machine q35,accel=tcg \
  -cpu qemu64 \
  -m 256M \
  -drive if=pflash,format=raw,readonly=on,file="$CODE_ARG" \
  -drive if=pflash,format=raw,file="$VARS_ARG" \
  -drive if=none,id=helixesp,format=raw,file="$DISK_ARG" \
  -device ich9-ahci,id=ahci \
  -device ide-hd,drive=helixesp,bus=ahci.0,bootindex=1 \
  -netdev user,id=net0,net=10.0.2.0/24,host=10.0.2.2,hostfwd=tcp::8080-:8080 \
  -device e1000,netdev=net0 \
  -chardev stdio,id=com1,logfile="$LOG_ARG",signal=off \
  -serial chardev:com1 \
  "${QEMU_DISPLAY_ARGS[@]}" \
  -name "HelixOS-M7" \
  "$@"
