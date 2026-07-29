# user/ — freestanding test programs

| File | Role |
|------|------|
| `init.c` / `task2.c` | M3 cooperative demo |
| `helixbox.c` | M5 multi-call：echo cat ls uname sh smoke |
| `usys.h` / `*.ld` | syscall helpers + link scripts |

`make user` → ELFs on ESP via mkesp. helixbox is **MIT**, not BusyBox GPL — see `third_party/README.md`.
