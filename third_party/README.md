# third_party/

Vendored or referenced third-party software. **Original licenses apply.**

## BusyBox (planned / optional)

| Item | Value |
|------|-------|
| Upstream | https://busybox.net/ |
| License | **GPL-2.0-only** |
| Helix status | **Not vendored in-tree for M5** (no Linux musl cross-compiler on the primary Windows/MSYS2 dev path) |

### How to obtain source (GPL compliance when you ship BusyBox)

When you build and distribute a BusyBox binary for HelixOS:

1. Download the **exact** upstream version you built, e.g.  
   `https://busybox.net/downloads/busybox-1.36.1.tar.bz2`
2. Keep that tarball or a git tag under `third_party/busybox/` **or** publish a clearly written offer for Corresponding Source (GPLv2 §3).
3. Document the `.config` used (`CONFIG_STATIC=y`, applet set).
4. Do **not** link BusyBox objects into MIT-only Helix kernel binaries; keep user ELF separate on the ESP.

### Suggested build (Linux/WSL with musl)

```bash
# example — adjust paths/version
curl -LO https://busybox.net/downloads/busybox-1.36.1.tar.bz2
tar xf busybox-1.36.1.tar.bz2 && cd busybox-1.36.1
make defconfig
# enable STATIC, trim applets to echo cat ls uname sh
make CROSS_COMPILE=x86_64-linux-musl- CONFIG_STATIC=y -j$(nproc)
cp busybox /path/to/HelixOS/build/user/  # then mkesp --add …:bin/busybox
```

## Helixbox (in-tree, M5)

| Item | Value |
|------|-------|
| Path | `user/helixbox.c` |
| License | **MIT** (HelixOS first-party) |
| Role | Multi-call stand-in speaking **Linux x86_64 syscall numbers** for smoke-linux |

Helixbox is **not** BusyBox and is not GPL. It exists so M5 can be verified without a GPL cross build on every developer machine. Replacing `/bin/helixbox` with a real static BusyBox is a drop-in goal once toolchain is available—expect more syscalls to light up via `ENOSYS` logs.

## First-party

All Helix kernel and `user/init`/`task2`/`helixbox` code: MIT (`LICENSE` at repo root).
