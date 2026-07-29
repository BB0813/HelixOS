# Goal: HelixOS M6 — 动态链接与 musl

> **状态：已完成（最小，2026-07-29）** — 自研 `ld-helix` + `PT_INTERP` + `hello.dyn`；  
> 验证：`make smoke-dyn` → `HelloDynOK`。完整 musl `ld.so` 未宣称（Windows/MSYS 无 musl 交叉链）。

实现：`kernel/proc/elf.c`（`elf_load_dynamic`）、`user/ld_helix.c`、`user/hello_dyn.c`、`scripts/elf_set_interp.py`；  
syscall：`mmap`(匿名/FIXED hint)/`munmap`/`mprotect`/`arch_prctl` 等。

1. **动态 ELF 加载**  
   - 识别 `PT_INTERP`；加载 interpreter（musl `ld.so` 或自研最小 loader）  
   - 加载主程序 `PT_LOAD`（可 PIE/`ET_DYN`）；处理基本 relocations 所需入口条件  
   - aux vector 补全：`AT_PHDR`/`AT_PHENT`/`AT_PHNUM`/`AT_ENTRY`/`AT_BASE`（interp base）/`AT_PAGESZ`/`AT_RANDOM`/`AT_EXECFN` 等最小集  
   - 栈上 argv/envp/auxv 布局符合 Linux x86_64  
   - 文档写清：用系统 musl 动态程序 **或** 仓库内构建的 test-dyn

2. **Syscall 按真实缺口扩展（禁止空抄）**  
   - 以动态 hello 的 strace/ENOSYS 日志为准  
   - 预期刚需：完善 `mmap`（含 FIXED/匿名/可能 file-backed 只读）、`munmap`、`mprotect`；`brk`；`arch_prctl`；可能 `openat`/`readlink`/`readlinkat`（`/etc/ld-musl-x86_64.path` 等可 stub）、`stat`/`fstat`/`newfstatat`、`close`、`write`  
   - 未知号继续 `-ENOSYS`；全部写入 `SYSCALLS.md`（编号、偏差）

3. **构建与测资**  
   - 文档化如何得到动态测试 ELF + ld.so（Linux/WSL musl 交叉，或下载可复现产物并校验哈希）  
   - 放入 ESP（如 `/lib/ld-musl-x86_64.so.1`、`/bin/hello.dyn`）  
   - Windows/MSYS 主路径若无法交叉：允许「预置二进制 + 脚本 fetch」与 helixbox 类似；合规/来源写清

4. **回归与自动化**  
   - 既有 smoke 不破  
   - 新增 **`make smoke-dyn`**（名可改）：串口出现动态程序输出（约定如 `HelloDynOK`）  
   - 若动态路径因工具链缺失跳过：ROADMAP 标明 blocked 原因，且至少完成 loader 骨架 + 单测/文档；**优先尽量跑通**

5. **文档**  
   - `SYSCALLS.md` 同步；`ARCHITECTURE` 写动态加载路径；`BUILD` 写交叉/fetch 步骤；`ROADMAP` 勾选；`README` 状态

## 约束
C+极少 asm；现有 clang/lld/QEMU 链；串口验证；不引入 Linux **内核**源码；用户指针校验；少空谈；简体中文+英文标识符；动态库 GPL/LGPL 若 vendoring 放 `third_party/` 并说明。

## 验收
- `make smoke && make smoke-user && make smoke-fs && make smoke-linux` 通过  
- **`make smoke-dyn`**：动态程序用户态输出可 grep（或文档+代码证明 loader 已加载 interp 并进入用户入口且有明确中间日志——仅当工具链不可得时的降级，需用户同意倾向）  
- `SYSCALLS.md` 含本阶段新增 mmap/动态相关项  
- BUILD 可复现获取/构建动态测资  

## 非目标
完整 glibc；全 Linux ld 行为；网络/图形（M7）；FAT 完整写（可继续只用 ramfs）；桌面。

## 顺序
1. 准备最小 musl 动态 hello + ld.so（或 fetch）  
2. ENOSYS 驱动补 syscall  
3. 加载 PT_INTERP + 主程序 + auxv  
4. smoke-dyn + 回归 + 文档  

## 开工
先读 elf/exec/syscall/mmap 现状与 GOAL_M5；用真实动态 ELF 缺口驱动。阶段末给验证命令与 serial 摘录。
