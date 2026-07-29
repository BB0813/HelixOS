# libk/ — freestanding kernel support library

No Boot Services dependency. Safe before and after `ExitBootServices`.

| File | API |
|------|-----|
| `serial.c` | COM1 `serial_init` / `putchar` / `puts` |
| `kprintf.c` | `kprintf` / `kvprintf` (`%s%c%d%u%x%llx%p`) |
| `panic.c` | `panic` (noreturn), used by `assert` |
| `string.c` | `memcpy` `memset` `memcmp` `memmove` `strlen` |
