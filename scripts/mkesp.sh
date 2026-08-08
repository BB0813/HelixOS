#!/usr/bin/env bash
# mkesp.sh — stage ESP dir + build bootable GPT disk image (out/esp.img)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ESP_DIR="${ESP_DIR:-$ROOT/esp}"
IMG="${ESP_IMG:-$ROOT/out/esp.img}"
EFI_SRC="${EFI_SRC:-$ESP_DIR/EFI/BOOT/BOOTX64.EFI}"

if [[ -d /c/msys64/mingw64/bin ]]; then
  export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
elif [[ -d /mingw64/bin ]]; then
  export PATH="/mingw64/bin:/usr/bin:$PATH"
fi

if [[ ! -f "$EFI_SRC" ]]; then
  if [[ -f "$ROOT/out/BOOTX64.EFI" ]]; then
    mkdir -p "$ESP_DIR/EFI/BOOT"
    cp "$ROOT/out/BOOTX64.EFI" "$ESP_DIR/EFI/BOOT/BOOTX64.EFI"
    EFI_SRC="$ESP_DIR/EFI/BOOT/BOOTX64.EFI"
  else
    echo "error: BOOTX64.EFI not found — run 'make' first" >&2
    exit 1
  fi
fi

mkdir -p "$(dirname "$IMG")" "$ESP_DIR/EFI/BOOT"
if [[ "$(cd "$(dirname "$EFI_SRC")" && pwd)/$(basename "$EFI_SRC")" != \
      "$(cd "$ESP_DIR/EFI/BOOT" && pwd)/BOOTX64.EFI" ]]; then
  cp -f "$EFI_SRC" "$ESP_DIR/EFI/BOOT/BOOTX64.EFI"
  EFI_SRC="$ESP_DIR/EFI/BOOT/BOOTX64.EFI"
fi

# Optional payload files for M4+ (use paths relative to ROOT so
# native Windows python.exe can open them under MSYS).
ADD_ARGS=()
cd "$ROOT"
if [[ -f esp_assets/hello.txt ]]; then
  ADD_ARGS+=(--add "esp_assets/hello.txt:hello.txt")
fi
# M20: extra smoke assets for cat /etc/* tests
if [[ -f esp_assets/welcome.txt ]]; then
  ADD_ARGS+=(--add "esp_assets/welcome.txt:etc/welcome.txt")
fi
if [[ -f esp_assets/passwd ]]; then
  ADD_ARGS+=(--add "esp_assets/passwd:etc/passwd")
fi
if [[ -f build/user/init.elf ]]; then
  ADD_ARGS+=(--add "build/user/init.elf:bin/init.elf")
else
  echo "error: build/user/init.elf missing — run 'make user' first" >&2
  exit 1
fi
if [[ -f build/user/task2.elf ]]; then
  ADD_ARGS+=(--add "build/user/task2.elf:bin/task2.elf")
else
  echo "error: build/user/task2.elf missing — run 'make user' first" >&2
  exit 1
fi
if [[ -f build/user/helixbox.elf ]]; then
  ADD_ARGS+=(--add "build/user/helixbox.elf:bin/helixbox")
else
  echo "error: build/user/helixbox.elf missing — run 'make user' first" >&2
  exit 1
fi
if [[ -f build/user/msh.elf ]]; then
  ADD_ARGS+=(--add "build/user/msh.elf:bin/msh")
else
  echo "error: build/user/msh.elf missing — run 'make user' first" >&2
  exit 1
fi
if [[ -f build/user/tui.elf ]]; then
  ADD_ARGS+=(--add "build/user/tui.elf:bin/tui")
else
  echo "error: build/user/tui.elf missing — run 'make user' first" >&2
  exit 1
fi
if [[ -f build/user/ld-helix.so ]]; then
  ADD_ARGS+=(--add "build/user/ld-helix.so:lib/ld-helix.so")
fi
if [[ -f build/user/hello.dyn ]]; then
  ADD_ARGS+=(--add "build/user/hello.dyn:bin/hello.dyn")
fi
# Official static BusyBox (optional)
if [[ -f third_party/busybox/busybox ]]; then
  ADD_ARGS+=(--add "third_party/busybox/busybox:bin/busybox")
fi
# musl dynamic (NAS-built; optional)
if [[ -f third_party/musl-dyn/ld-musl-x86_64.so.1 ]]; then
  ADD_ARGS+=(--add "third_party/musl-dyn/ld-musl-x86_64.so.1:lib/ld-musl-x86_64.so.1")
  # musl unifies loader+libc; NEEDED "libc.so" must resolve to same image
  ADD_ARGS+=(--add "third_party/musl-dyn/ld-musl-x86_64.so.1:lib/libc.so")
fi
if [[ -f third_party/musl-dyn/hello.musl ]]; then
  ADD_ARGS+=(--add "third_party/musl-dyn/hello.musl:bin/hello.musl")
fi

PY="$(command -v python3 || command -v python || true)"
if [[ -z "$PY" ]]; then
  echo "error: python required to build GPT ESP image" >&2
  exit 1
fi

echo "mkesp: GPT+FAT ESP → $IMG"
"$PY" scripts/mkdisk.py \
  --efi "esp/EFI/BOOT/BOOTX64.EFI" \
  --out "out/esp.img" \
  --disk-mib "${DISK_MIB:-64}" \
  --esp-mib "${ESP_MIB:-32}" \
  "${ADD_ARGS[@]+"${ADD_ARGS[@]}"}"

ls -la out/esp.img
