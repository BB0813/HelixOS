# Goal: HelixOS M8 — Network syscall + UDP userland (M9 graphics optional)

## Project
HelixOS: self-written x86_64 UEFI kernel + Linux userspace ABI compatibility. M0–M7 done: boot/early kernel/shell/Ring3; AHCI+FAT; Linux syscall subset + helixbox/BusyBox; e1000 + ARP/IPv4/ICMP → HelixNetOK (make smoke-net).

**尚无**：network syscall, socket, TCP, GUI framebuffer.

QEMU: e1000 + user net; no socket syscall.

## This goal (one complete, buildable/verified)

On top of M7 establish **minimal network path** (graphics optional, does not block CLI):

1. **Network syscall**  
   - `socket` / `bind` / `sendto` / `recvfrom` (UDP)  
   - TCP stub / -ENOSYS  
   - Linux syscall numbers  
   - Unknown → -ENOSYS; update SYSCALLS.md

2. **Userland visibility**  
   - helixbox applet: `udpecho` / `ping` (or freestanding)  
   - Kernel self-test ICMP already present; optional user UDP echo  
   - `make smoke-net` upgraded to userland test

3. **Build & smoke**  
   - `make smoke-net` (name optional): serial contains network logs + `HelixNetOK`  
   - Existing smoke* targets unchanged  
   - Windows/QEMU user networking documented in BUILD.md

4. **Graphics (optional)**  
   - GOP/virtio-gpu simple framebuffer (clear screen + character)  
   - Not required; `smoke-fb` separate target  
   - Does not block network acceptance

5. **Documentation**  
   - ROADMAP: M8 check + ARCHITECTURE network stack boundary  
   - BUILD: QEMU network parameters  
   - SYSCALLS: add socket class  
   - README: status

## Constraints
- C + minimal asm; existing freestanding chain; serial still main log
- No Linux kernel source; reference specs only
- Interrupt context not re-block; receive can poll in idle/syscall
- Brief Chinese + English identifiers
- Third-party in third_party/ with license noted

## Acceptance
- `make && make smoke-net` passes (serial marks like `HelixNetOK`)
- Regression: smoke-dyn/musl still green
- QEMU command copyable; SYSCALLS updated
- Kernel ICMP sufficient; user UDP echo bonus

## Non-goals
- Full TCP (HTTP server later)
- DHCP client complete (static IP ok)
- IPv6, firewall, multiple NIC, Wi-Fi
- Desktop GUI / Wayland / GPU accel
- Replace M7 musl loader

## Suggested order
1. QEMU + e1000 + PCI probe + RX/TX ring
2. ARP + IPv4 + ICMP
3. socket syscall + userland applet
4. smoke-net + regression + docs

## Opening
First read scripts/run-qemu.sh, e1000 driver, idle loop and existing smoke; network with **ping or UDP echo** as first green. At end give verification command and serial.log excerpt.