# HelixOS top-level build (M0–M7)
#
# Produces: out/BOOTX64.EFI (+ embedded user ELFs)
# ESP also gets hello.txt, bin/*, lib/ld-helix.so, bin/hello.dyn

ifeq ($(origin CC),default)
  CC := clang
endif
ifeq ($(origin CC),environment)
  ifneq ($(findstring clang,$(CC)),clang)
    CC := clang
  endif
endif
CC       ?= clang
LLD_LINK ?= lld-link
LD_LLD   ?= ld.lld
MKDIR    := mkdir -p
RM_RF    := rm -rf
PYTHON   ?= python3

TARGET   := x86_64-unknown-windows
USER_TARGET := x86_64-unknown-none-elf

ROOT  := $(abspath .)
BUILD := $(ROOT)/build
OUT   := $(ROOT)/out
ESP   := $(ROOT)/esp
GEN   := $(ROOT)/include/generated

INCLUDES := -I$(ROOT)/include

CFLAGS := --target=$(TARGET) \
	-std=c11 -ffreestanding -fno-stack-protector -fno-stack-check \
	-fshort-wchar -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
	-fno-builtin -fno-PIC -Wall -Wextra -Werror -Os $(INCLUDES)

AFLAGS := --target=$(TARGET) -ffreestanding -c

LDFLAGS := -subsystem:efi_application -entry:efi_main -nodefaultlib

USER_CFLAGS := --target=$(USER_TARGET) -std=c11 -ffreestanding \
	-fno-stack-protector -fno-pic -mno-red-zone -fno-builtin \
	-Wall -Wextra -Werror -Os -I$(ROOT)/user

C_SRCS := \
	boot/efi_main.c \
	libk/serial.c libk/string.c libk/kprintf.c libk/panic.c \
	kernel/ke/main.c kernel/ke/shell.c \
	kernel/mm/pmm.c kernel/mm/heap.c kernel/mm/vmm.c \
	kernel/arch/x86_64/paging.c kernel/arch/x86_64/gdt.c \
	kernel/arch/x86_64/idt.c kernel/arch/x86_64/pic.c \
	kernel/arch/x86_64/pit.c kernel/arch/x86_64/timer.c \
	kernel/arch/x86_64/irq.c \
	kernel/drv/blk_ahci.c kernel/drv/virtio_net.c kernel/drv/e1000.c kernel/drv/fb.c \
	kernel/net/nic.c kernel/net/net.c kernel/net/udp.c kernel/net/tcp.c \
	kernel/fs/fat.c kernel/fs/vfs.c kernel/fs/fs.c kernel/fs/ramfs.c kernel/fs/pipe.c \
	kernel/proc/elf.c kernel/proc/syscall.c kernel/proc/task.c \
	kernel/proc/signal.c kernel/proc/userland.c kernel/proc/exec.c

S_SRCS := \
	kernel/arch/x86_64/isr_stubs.S \
	kernel/arch/x86_64/syscall_entry.S

C_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS))
S_OBJS := $(patsubst %.S,$(BUILD)/%.o,$(S_SRCS))
OBJS   := $(C_OBJS) $(S_OBJS)

USER_INIT_ELF  := $(BUILD)/user/init.elf
USER_TASK2_ELF := $(BUILD)/user/task2.elf
USER_BOX_ELF   := $(BUILD)/user/helixbox.elf
USER_LD_ELF    := $(BUILD)/user/ld-helix.so
USER_DYN_ELF   := $(BUILD)/user/hello.dyn
USER_MSH_ELF   := $(BUILD)/user/msh.elf
USER_INIT_HDR  := $(GEN)/user_init_elf.h
USER_TASK2_HDR := $(GEN)/user_task2_elf.h

ifdef HELIX_M1_TEST_PF
  CFLAGS += -DHELIX_M1_TEST_PF
endif

.PHONY: all clean esp run user check-deps dirs help fetch-busybox \
	smoke smoke-user smoke-fs smoke-linux smoke-dyn smoke-shell smoke-panic \
	smoke-net smoke-fb

all: $(OUT)/BOOTX64.EFI

help:
	@echo "HelixOS: all user esp run smoke* smoke-fb smoke-net clean"

dirs:
	$(MKDIR) $(BUILD)/boot $(BUILD)/libk $(BUILD)/kernel/ke $(BUILD)/kernel/mm \
		$(BUILD)/kernel/arch/x86_64 $(BUILD)/kernel/proc $(BUILD)/kernel/drv \
		$(BUILD)/kernel/fs $(BUILD)/kernel/net $(BUILD)/user $(GEN) $(OUT)

$(BUILD)/user/init.o: user/init.c user/usys.h | dirs
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/user/task2.o: user/task2.c user/usys.h | dirs
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/user/helixbox.o: user/helixbox.c user/usys.h | dirs
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/user/ld_helix.o: user/ld_helix.c user/usys.h | dirs
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/user/hello_dyn.o: user/hello_dyn.c user/usys.h | dirs
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/user/msh.o: user/msh.c user/usys.h | dirs
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_INIT_ELF): $(BUILD)/user/init.o user/user_init.ld
	$(LD_LLD) -m elf_x86_64 -static -nostdlib -T user/user_init.ld -o $@ $<
$(USER_TASK2_ELF): $(BUILD)/user/task2.o user/user_task2.ld
	$(LD_LLD) -m elf_x86_64 -static -nostdlib -T user/user_task2.ld -o $@ $<
$(USER_BOX_ELF): $(BUILD)/user/helixbox.o user/helixbox.ld
	$(LD_LLD) -m elf_x86_64 -static -nostdlib -T user/helixbox.ld -o $@ $<
$(USER_LD_ELF): $(BUILD)/user/ld_helix.o user/ld_helix.ld
	$(LD_LLD) -m elf_x86_64 -static -nostdlib -T user/ld_helix.ld -o $@ $<
$(USER_MSH_ELF): $(BUILD)/user/msh.o user/msh.ld
	$(LD_LLD) -m elf_x86_64 -static -nostdlib -T user/msh.ld -o $@ $<

$(BUILD)/user/hello_dyn.raw: $(BUILD)/user/hello_dyn.o user/hello_dyn.ld
	$(LD_LLD) -m elf_x86_64 -static -nostdlib -T user/hello_dyn.ld -o $@ $<

$(USER_DYN_ELF): $(BUILD)/user/hello_dyn.raw scripts/elf_set_interp.py
	@# Use relative paths: Windows python cannot open MSYS /z/... absolute paths.
	@# MSYS2_ARG_CONV_EXCL keeps --interp as Unix /lib/ld-helix.so (not C:/msys64/...).
	MSYS2_ARG_CONV_EXCL='*' $(PYTHON) scripts/elf_set_interp.py build/user/hello_dyn.raw -o build/user/hello.dyn --interp /lib/ld-helix.so

$(USER_INIT_HDR): $(USER_INIT_ELF) scripts/bin2hdr.py
	$(PYTHON) scripts/bin2hdr.py build/user/init.elf -o include/generated/user_init_elf.h -n user_init_elf
$(USER_TASK2_HDR): $(USER_TASK2_ELF) scripts/bin2hdr.py
	$(PYTHON) scripts/bin2hdr.py build/user/task2.elf -o include/generated/user_task2_elf.h -n user_task2_elf

user: $(USER_INIT_ELF) $(USER_TASK2_ELF) $(USER_BOX_ELF) $(USER_LD_ELF) $(USER_DYN_ELF) $(USER_MSH_ELF) \
	$(USER_INIT_HDR) $(USER_TASK2_HDR)

$(BUILD)/kernel/proc/userland.o: $(USER_INIT_HDR) $(USER_TASK2_HDR)

$(BUILD)/%.o: %.c | dirs
	$(MKDIR) $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S | dirs
	$(MKDIR) $(dir $@)
	TMPDIR="$${TMPDIR:-/tmp}" TEMP="$${TEMP:-$${TMPDIR}}" $(CC) $(AFLAGS) $< -o $@

$(OUT)/BOOTX64.EFI: $(OBJS) | dirs
	$(LLD_LINK) $(LDFLAGS) -out:out/BOOTX64.EFI $(OBJS)
	@echo "built $@"

esp: $(OUT)/BOOTX64.EFI user
	$(MKDIR) $(ESP)/EFI/BOOT
	cp $(OUT)/BOOTX64.EFI $(ESP)/EFI/BOOT/BOOTX64.EFI
	@echo "ESP staged at $(ESP)"

run: esp
	@bash $(ROOT)/scripts/run-qemu.sh

smoke: esp
	@rm -f $(ROOT)/serial.log $(ROOT)/ovmf_vars.fd
	@HEADLESS=1 TIMEOUT_SECS=30 bash $(ROOT)/scripts/run-qemu.sh || true
	@ok=1; for pat in "Helix boot" "ExitBootServices OK" "M1 early kernel OK" "M2 shell ready" "[tick]"; do \
		grep -a -F -q "$$pat" $(ROOT)/serial.log 2>/dev/null || { echo "SMOKE FAIL $$pat"; ok=0; }; done; \
	[ "$$ok" = 1 ] && echo SMOKE OK || { cat $(ROOT)/serial.log; exit 1; }

smoke-user: esp
	@rm -f $(ROOT)/serial.log $(ROOT)/ovmf_vars.fd
	@HEADLESS=1 TIMEOUT_SECS=40 bash $(ROOT)/scripts/run-qemu.sh || true
	@ok=1; for pat in "Hello from Ring3" "M3 userland OK"; do \
		grep -a -F -q "$$pat" $(ROOT)/serial.log 2>/dev/null || { echo "SMOKE-USER FAIL $$pat"; ok=0; }; done; \
	[ "$$ok" = 1 ] && echo SMOKE-USER OK || { cat $(ROOT)/serial.log; exit 1; }

smoke-fs: esp
	@rm -f $(ROOT)/serial.log $(ROOT)/ovmf_vars.fd
	@HEADLESS=1 TIMEOUT_SECS=45 bash $(ROOT)/scripts/run-qemu.sh || true
	@ok=1; for pat in "M4 fs ready" "HelixFS OK" "HelixFATWriteOK" "loaded init+task2 from disk"; do \
		grep -a -F -q "$$pat" $(ROOT)/serial.log 2>/dev/null || { echo "SMOKE-FS FAIL $$pat"; ok=0; }; done; \
	if [ "$$ok" = 1 ]; then echo SMOKE-FS OK; \
		grep -a -E '\[fat\]|\[fs\]|HelixFS|HelixFAT|write/mkdir' $(ROOT)/serial.log | head -40; \
	else cat $(ROOT)/serial.log; exit 1; fi

smoke-linux: esp
	@rm -f $(ROOT)/serial.log $(ROOT)/ovmf_vars.fd
	@HEADLESS=1 TIMEOUT_SECS=55 bash $(ROOT)/scripts/run-qemu.sh || true
	@ok=1; 	if grep -a -F -q "HelixBusyBoxOK" $(ROOT)/serial.log 2>/dev/null; then 		echo "SMOKE-LINUX OK (BusyBox)"; 		grep -a -E 'BusyBox|HelixBusy|helixbox|HelixLinux|tmp_write' $(ROOT)/serial.log | head -40; 	else 		for pat in "HelixLinuxOK" "helixbox_smoke_done" "tmp_write_ok"; do 			grep -a -F -q "$$pat" $(ROOT)/serial.log 2>/dev/null || { echo "SMOKE-LINUX FAIL $$pat"; ok=0; }; 		done; 		[ "$$ok" = 1 ] && echo "SMOKE-LINUX OK (helixbox)" || { cat $(ROOT)/serial.log; exit 1; }; 	fi; 	if grep -a -F -q "HelixMshOK" $(ROOT)/serial.log 2>/dev/null; then 		echo "SMOKE-LINUX OK (msh pipeline)"; 	fi; 	if grep -a -F -q "HelixCwdOK" $(ROOT)/serial.log 2>/dev/null; then 		echo "SMOKE-LINUX OK (cwd/chdir)"; 	else 		echo "SMOKE-LINUX WARN: HelixCwdOK missing"; 	fi; 	if grep -a -F -q "HelixSigOK" $(ROOT)/serial.log 2>/dev/null; then 		echo "SMOKE-LINUX OK (signals)"; 	else 		echo "SMOKE-LINUX WARN: HelixSigOK missing"; 	fi

smoke-musl: esp
	@rm -f $(ROOT)/serial.log $(ROOT)/ovmf_vars.fd
	@HEADLESS=1 TIMEOUT_SECS=60 bash $(ROOT)/scripts/run-qemu.sh || true
	@ok=1; for pat in "M6 musl" "HelloMuslDynOK" "ld-musl-x86_64.so.1"; do \
		grep -a -F -q "$$pat" $(ROOT)/serial.log 2>/dev/null || { echo "SMOKE-MUSL FAIL $$pat"; ok=0; }; done; \
	if [ "$$ok" = 1 ]; then echo SMOKE-MUSL OK; \
		grep -a -E '\[musl\]|\[elf\]|HelloMuslDynOK|ld-musl|PT_INTERP' $(ROOT)/serial.log | head -40; \
	else cat $(ROOT)/serial.log; exit 1; fi

fetch-busybox:
	@bash $(ROOT)/scripts/fetch-busybox.sh

smoke-shell: esp
	@bash $(ROOT)/scripts/smoke-shell.sh

smoke-panic:
	$(RM_RF) $(BUILD)/kernel/ke $(OUT)/BOOTX64.EFI
	$(MAKE) all HELIX_M1_TEST_PF=1
	$(MAKE) esp
	@rm -f $(ROOT)/serial.log $(ROOT)/ovmf_vars.fd
	@HEADLESS=1 TIMEOUT_SECS=25 bash $(ROOT)/scripts/run-qemu.sh || true
	@grep -a -q "HELIX PANIC" $(ROOT)/serial.log && echo SMOKE-PANIC OK || exit 1
	$(RM_RF) $(BUILD)/kernel/ke $(OUT)/BOOTX64.EFI
	$(MAKE) all

check-deps:
	@bash $(ROOT)/scripts/check-deps.sh

clean:
	$(RM_RF) $(BUILD) $(OUT) $(ESP) $(GEN) $(ROOT)/serial.log $(ROOT)/ovmf_vars.fd

smoke-net: esp
	@rm -f $(ROOT)/serial.log $(ROOT)/ovmf_vars.fd
	@python $(ROOT)/scripts/tcp_echo_server.py & TCP_PID=$$!; sleep 1; HEADLESS=1 TIMEOUT_SECS=120 bash $(ROOT)/scripts/run-qemu.sh || true; kill $$TCP_PID 2>/dev/null || true
	@grep -a -F -q "M7 net ready" $(ROOT)/serial.log || { echo SMOKE-NET FAIL M7; cat $(ROOT)/serial.log; exit 1; }
	@grep -a -F -q "10.0.2.15" $(ROOT)/serial.log || { echo SMOKE-NET FAIL IP; exit 1; }
	@grep -a -F -q "HelixNetOK" $(ROOT)/serial.log || { echo SMOKE-NET FAIL HelixNetOK; cat $(ROOT)/serial.log; exit 1; }
	@grep -a -F -q "ICMP echo reply" $(ROOT)/serial.log || { echo SMOKE-NET FAIL ICMP; exit 1; }
	@grep -a -F -q "user_udp_ok" $(ROOT)/serial.log || { echo SMOKE-NET FAIL user_udp_ok; cat $(ROOT)/serial.log; exit 1; }
	@grep -a -F -q "HelixTcpOK" $(ROOT)/serial.log || { echo SMOKE-NET FAIL HelixTcpOK; cat $(ROOT)/serial.log; exit 1; }
	@if grep -a -F -q "HelixTcpUserOK" $(ROOT)/serial.log 2>/dev/null; then echo "SMOKE-NET OK (TCP user)"; else echo "SMOKE-NET WARN: HelixTcpUserOK missing"; fi
	@echo SMOKE-NET OK
	@grep -a -E 'net|arp|icmp|udp|tcp|HelixNet|host_udp' $(ROOT)/serial.log | head -40

smoke-fb: esp
	@rm -f $(ROOT)/serial.log $(ROOT)/ovmf_vars.fd
	@HEADLESS=0 TIMEOUT_SECS=35 bash $(ROOT)/scripts/run-qemu.sh || true
	@if grep -a -F -q "fb_smoke_done" $(ROOT)/serial.log 2>/dev/null; then \
		echo "SMOKE-FB OK (framebuffer)"; \
	elif grep -a -F -q "M9 no framebuffer" $(ROOT)/serial.log 2>/dev/null; then \
		echo "SMOKE-FB OK (no GOP — headless fallback, OVMF lacks QemuVideoDxe)"; \
	else \
		echo "SMOKE-FB FAIL"; cat $(ROOT)/serial.log; exit 1; \
	fi
