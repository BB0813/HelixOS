/*
 * HelixOS boot — UEFI entry (M1)
 *
 * Responsibilities (boot/ only):
 *   - early serial + banner (M0 compatible)
 *   - GetMemoryMap / copy into LoaderData
 *   - ExitBootServices
 *   - hand off to kernel_early_main()  (no return to firmware)
 *
 * After ExitBootServices this file must not call Boot Services.
 */

#include "efi/efi.h"
#include "helix/boot_info.h"
#include "helix/serial.h"
#include "helix/kprintf.h"
#include "helix/string.h"
#include "helix/types.h"

extern void kernel_early_main(struct helix_boot_info *info);

static void efi_puts(EFI_SYSTEM_TABLE *st, CHAR16 *s)
{
    if (st && st->ConOut && st->ConOut->OutputString)
        st->ConOut->OutputString(st->ConOut, s);
}

/* Fetch memory map into a caller-owned pool buffer. Retries on TOO_SMALL. */
static EFI_STATUS get_memory_map(
    EFI_BOOT_SERVICES *bs,
    EFI_MEMORY_DESCRIPTOR **out_map,
    UINTN *out_size,
    UINTN *out_key,
    UINTN *out_desc_size,
    uint32_t *out_desc_ver)
{
    UINTN size = 0;
    UINTN key = 0;
    UINTN desc_size = 0;
    uint32_t desc_ver = 0;
    EFI_STATUS st;
    EFI_MEMORY_DESCRIPTOR *map = 0;

    st = bs->GetMemoryMap(&size, map, &key, &desc_size, &desc_ver);
    if (st != EFI_BUFFER_TOO_SMALL && EFI_ERROR(st))
        return st;

    /* slack for the AllocatePool entry itself */
    size += desc_size * 16;

    st = bs->AllocatePool(EfiLoaderData, size, (void **)&map);
    if (EFI_ERROR(st))
        return st;

    st = bs->GetMemoryMap(&size, map, &key, &desc_size, &desc_ver);
    if (EFI_ERROR(st)) {
        bs->FreePool(map);
        return st;
    }

    *out_map = map;
    *out_size = size;
    *out_key = key;
    *out_desc_size = desc_size;
    *out_desc_ver = desc_ver;
    return EFI_SUCCESS;
}

/* Copy UEFI map into a compact helix_mmap_entry array (also LoaderData). */
static EFI_STATUS copy_mmap(
    EFI_BOOT_SERVICES *bs,
    EFI_MEMORY_DESCRIPTOR *map,
    UINTN map_size,
    UINTN desc_size,
    struct helix_mmap_entry **out_entries,
    u64 *out_count,
    u64 *out_ceiling)
{
    UINTN n = map_size / desc_size;
    struct helix_mmap_entry *ents = 0;
    EFI_STATUS st = bs->AllocatePool(
        EfiLoaderData, n * sizeof(struct helix_mmap_entry), (void **)&ents);
    if (EFI_ERROR(st))
        return st;

    /* phys_ceiling = top of actual RAM-like regions only.
     * Do NOT include MMIO/Reserved that often extend to 1TiB+ on QEMU —
     * that would explode the PMM bitmap and identity map. */
    u64 ceiling = 0;
    u8 *p = (u8 *)map;
    for (UINTN i = 0; i < n; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(p + i * desc_size);
        ents[i].type = d->Type;
        ents[i].pad = 0;
        ents[i].phys_start = d->PhysicalStart;
        ents[i].virt_start = d->VirtualStart;
        ents[i].npages = d->NumberOfPages;
        ents[i].attrs = d->Attribute;

        u32 t = d->Type;
        int ramish = (t == EfiLoaderCode || t == EfiLoaderData
                      || t == EfiBootServicesCode || t == EfiBootServicesData
                      || t == EfiRuntimeServicesCode || t == EfiRuntimeServicesData
                      || t == EfiConventionalMemory || t == EfiACPIReclaimMemory
                      || t == EfiACPIMemoryNVS || t == EfiPersistentMemory);
        if (ramish) {
            u64 end = d->PhysicalStart + d->NumberOfPages * 4096ull;
            if (end > ceiling)
                ceiling = end;
        }
    }
    *out_entries = ents;
    *out_count = (u64)n;
    *out_ceiling = ceiling;
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    serial_init();
    kprintf("[Helix] boot stub starting\n");
    kprintf("HelixOS M1 — UEFI boot\n");
    kprintf("Helix boot\n");

    if (!SystemTable || !SystemTable->BootServices) {
        kprintf("[Helix] FATAL: no BootServices\n");
        for (;;)
            __asm__ volatile("hlt");
    }

    if (SystemTable->ConOut) {
        if (SystemTable->ConOut->ClearScreen)
            SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
        efi_puts(SystemTable, L"HelixOS M1 boot\r\n");
    }

    EFI_BOOT_SERVICES *bs = SystemTable->BootServices;

    /* Disable watchdog */
    if (bs->SetWatchdogTimer)
        bs->SetWatchdogTimer(0, 0, 0, 0);

    EFI_MEMORY_DESCRIPTOR *map = 0;
    UINTN map_size = 0, map_key = 0, desc_size = 0;
    uint32_t desc_ver = 0;
    EFI_STATUS st;

    struct helix_mmap_entry *ents = 0;
    u64 ent_count = 0, ceiling = 0;

    /* Allocate boot_info in LoaderData before ExitBootServices */
    struct helix_boot_info *info = 0;
    st = bs->AllocatePool(EfiLoaderData, sizeof(*info), (void **)&info);
    if (EFI_ERROR(st)) {
        kprintf("[Helix] AllocatePool(boot_info) failed: 0x%llx\n",
                (unsigned long long)st);
        goto fail;
    }
    memset(info, 0, sizeof(*info));

    /* M9: Locate GOP and capture framebuffer before ExitBootServices.
     * First try LocateProtocol; if GOP isn't found, try recursively
     * connecting all controllers (loads video DXE drivers), then retry.
     * Some OVMF builds need an explicit ConnectController call to load
     * the video driver onto the VGA device. */
    {
        EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
        EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;

        st = bs->LocateProtocol(&gop_guid, 0, (void**)&gop);

        /* If GOP not found, recursively connect all controllers to load video drivers */
        if ((EFI_ERROR(st) || !gop) && bs->ConnectController) {
            kprintf("[Helix] GOP not found, trying ConnectController\n");
            bs->ConnectController(0, 0, 0, 1 /*Recursive*/);
            st = bs->LocateProtocol(&gop_guid, 0, (void**)&gop);
        }

        if (gop && gop->Mode) {
            st = EFI_SUCCESS;
            /* Find best mode with width ≥ 640 */
            uint32_t best = gop->Mode->Mode;
            uint32_t best_w = gop->Mode->Info->HorizontalResolution;
            for (uint32_t m = 0; m < gop->Mode->MaxMode; m++) {
                EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = 0;
                uint64_t sz = 0;
                if (gop->QueryMode(gop, m, &sz, &mi) == EFI_SUCCESS && mi) {
                    if (mi->HorizontalResolution >= 640 &&
                        mi->HorizontalResolution >= best_w) {
                        best = m;
                        best_w = mi->HorizontalResolution;
                    }
                }
            }
            if (best != gop->Mode->Mode)
                gop->SetMode(gop, best);
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = gop->Mode->Info;
            info->fb_addr   = gop->Mode->FrameBufferBase;
            info->fb_size   = gop->Mode->FrameBufferSize;
            info->fb_width  = mi->HorizontalResolution;
            info->fb_height = mi->VerticalResolution;
            info->fb_pitch  = mi->PixelsPerScanLine * 4;
            info->fb_bpp    = 32;
            kprintf("[Helix] GOP fb=0x%llx %ux%u pitch=%u\n",
                    (unsigned long long)info->fb_addr,
                    info->fb_width, info->fb_height, info->fb_pitch);
            /* Ensure phys_ceiling covers framebuffer so identity map includes it */
            u64 fb_end = info->fb_addr + info->fb_size;
            if (fb_end > ceiling)
                ceiling = fb_end;
        } else {
            kprintf("[Helix] GOP not found (no framebuffer)\n");
        }
    }

    /* Dedicated kernel stack (LoaderData pages) so we can leave the UEFI stack. */
    {
        EFI_PHYSICAL_ADDRESS stack_phys = 0;
        const UINTN stack_pages = 8; /* 32 KiB */
        st = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, stack_pages, &stack_phys);
        if (EFI_ERROR(st)) {
            kprintf("[Helix] AllocatePages(stack) failed: 0x%llx\n",
                    (unsigned long long)st);
            goto fail;
        }
        info->kernel_stack_top = (u64)stack_phys + (u64)stack_pages * 4096ull;
        kprintf("[Helix] kernel stack top=0x%llx\n",
                (unsigned long long)info->kernel_stack_top);
    }

    /* Get map → copy → ExitBootServices, retry a few times if key changes */
    for (int attempt = 0; attempt < 5; attempt++) {
        if (map) {
            bs->FreePool(map);
            map = 0;
        }
        if (ents) {
            bs->FreePool(ents);
            ents = 0;
        }

        st = get_memory_map(bs, &map, &map_size, &map_key, &desc_size, &desc_ver);
        if (EFI_ERROR(st)) {
            kprintf("[Helix] GetMemoryMap failed: 0x%llx\n", (unsigned long long)st);
            goto fail;
        }

        st = copy_mmap(bs, map, map_size, desc_size, &ents, &ent_count, &ceiling);
        if (EFI_ERROR(st)) {
            kprintf("[Helix] copy_mmap failed: 0x%llx\n", (unsigned long long)st);
            goto fail;
        }

        kprintf("[Helix] MemoryMap: %llu entries, desc_size=%llu, ceiling=0x%llx (try %d)\n",
                (unsigned long long)ent_count,
                (unsigned long long)desc_size,
                (unsigned long long)ceiling,
                attempt);

        /* Re-get map for a fresh key right before exit (copy already done) */
        {
            UINTN sz = map_size, key = 0, ds = 0;
            uint32_t dv = 0;
            st = bs->GetMemoryMap(&sz, map, &key, &ds, &dv);
            if (EFI_ERROR(st)) {
                /* buffer may be too small after copy_mmap allocations */
                bs->FreePool(map);
                map = 0;
                st = get_memory_map(bs, &map, &map_size, &map_key, &desc_size, &desc_ver);
                if (EFI_ERROR(st)) {
                    kprintf("[Helix] re-GetMemoryMap failed: 0x%llx\n",
                            (unsigned long long)st);
                    goto fail;
                }
                key = map_key;
            } else {
                map_key = key;
                map_size = sz;
                desc_size = ds;
            }
        }

        st = bs->ExitBootServices(ImageHandle, map_key);
        if (st == EFI_SUCCESS)
            break;

        kprintf("[Helix] ExitBootServices failed: 0x%llx, retrying\n",
                (unsigned long long)st);
    }

    if (EFI_ERROR(st)) {
        kprintf("[Helix] ExitBootServices giving up: 0x%llx\n",
                (unsigned long long)st);
        goto fail;
    }

    /* **** Boot Services are GONE. Only COM1 + our memory. **** */
    kprintf("[Helix] ExitBootServices OK\n");

    info->mmap = ents;
    info->mmap_count = ent_count;
    info->phys_ceiling = ceiling;
    info->image_base = 0;
    info->image_size = 0;

    /* Switch off the UEFI stack before touching the allocator. */
    if (info->kernel_stack_top) {
        u64 new_sp = info->kernel_stack_top & ~0xFull;
        __asm__ volatile(
            "mov %[sp], %%rsp\n\t"
            "mov %[info], %%rcx\n\t"
            "sub $32, %%rsp\n\t"
            "call kernel_early_main\n\t"
            "1: cli; hlt; jmp 1b\n\t"
            :
            : [sp] "r"(new_sp), [info] "r"(info)
            : "rcx", "memory");
    }

    kernel_early_main(info);

    /* no return */
    for (;;)
        __asm__ volatile("cli; hlt");

fail:
    kprintf("[Helix] boot failed — hanging\n");
    for (;;) {
        if (bs && bs->Stall)
            bs->Stall(1000000);
        else
            __asm__ volatile("hlt");
    }
    return EFI_LOAD_ERROR;
}
