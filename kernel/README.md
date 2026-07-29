# kernel/ — Helix early kernel (M1+)

After `ExitBootServices`, control lives here (still linked into the same
`BOOTX64.EFI` image for now).

| Path | Role |
|------|------|
| `ke/main.c` | `kernel_early_main` — bring-up sequence + idle |
| `mm/pmm.c` | physical page bitmap allocator |
| `mm/heap.c` | tiny `kmalloc`/`kfree` |
| `arch/x86_64/paging.c` | identity map with 2MiB pages |
| `arch/x86_64/idt.c` | IDT for vectors 0–31 |
| `arch/x86_64/isr_stubs.S` | exception entry stubs |

Boot firmware interaction stays in `boot/`. Shared utilities in `libk/`.
