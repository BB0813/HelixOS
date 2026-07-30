# Goal: HelixOS M6 完整 musl 动态链接

> **状态：进行中** — NAS (Debian 12) 上已成功构建 `hello.musl`；  
> 自研 `ld-helix` 作为 fallback 保留；真实 musl 动态链接为本阶段主线。

在 M6 自研 loader 基础上，用 NAS 交叉编译出真正的 musl `ld-musl-x86_64.so.1` +  
动态 `hello.musl`，部署到 ESP，让内核支撑 musl 的完整动态链接流程。

1. **NAS 交叉构建真实 musl 产物**  
   - NAS: `root@192.168.1.122`，Debian 12，`musl-tools` 已装  
   - 构建 `ld-musl-x86_64.so.1`（musl 官方 loader）和动态 `hello.musl`  
   - `paramiko` SSH 已通，构建脚本可复现，产物哈希记录  
   - 产物放置 ESP：`/lib/ld-musl-x86_64.so.1`、`/lib/libc.so`（symlink）、`/bin/hello.musl`

2. **内核补齐 musl 动态链接所需 syscall**  
   - 以 `hello.musl` 真实 strace/ENOSYS 为准驱动  
   - **mmap**：匿名/FIXED hint/file-backed 只读（ld.so 加载自身 + libc 需要）  
   - **mprotect**：真实 PTE 权限变更（PROT_READ/PROT_WRITE/PROT_EXEC）  
   - **brk**：堆初始化，musl `malloc` 依赖  
   - **arch_prctl**：FS/GSBASE，musl TLS 初始化需要（至少 stub 成功）  
   - 其余未知号 `-ENOSYS`，写入 `SYSCALLS.md`

3. **加载流程与 M6 自研 loader 共存**  
   - 内核识别 `PT_INTERP` → 加载 `ld-musl-x86_64.so.1`（真实 musl loader）  
   - auxv 完整：`AT_PHDR`/`AT_PHENT`/`AT_PHNUM`/`AT_ENTRY`/`AT_BASE`/`AT_PAGESZ`/`AT_RANDOM`/`AT_EXECFN`  
   - 自研 `ld-helix` 保留为 fallback（工具链不可得时降级路径）  
   - 栈布局符合 Linux x86_64 规范

4. **回归与自动化**  
   - 既有 smoke 全部不破（`smoke`/`smoke-user`/`smoke-fs`/`smoke-linux`/`smoke-dyn`）  
   - 新增 **`make smoke-musl`**：串口输出 `HelloMuslDynOK`（或类似约定标记）  
   - smoke-musl 验证：ld-musl 成功加载 → hello.musl 执行 → 输出可 grep  
   - 若 musl ld.so 本身触发额外 ENOSYS，逐个补齐而非 stub 绕过

5. **文档同步**  
   - `SYSCALLS.md`：本阶段新增 mprotect/brk/arch_prctl 详情  
   - `BUILD`：NAS 交叉构建步骤、ESP 部署方法  
   - `ROADMAP` 勾选 musl 动态链接  
   - `README` 状态更新

## 约束
C+极少 asm；现有 clang/lld/QEMU 链；串口验证；不引入 Linux **内核**源码；  
用户指针校验；简体中文+英文标识符；musl 为 MIT 许可，可直接用无需 `third_party/`。

## 验收
- `make smoke && make smoke-user && make smoke-fs && make smoke-linux && make smoke-dyn` 通过  
- **`make smoke-musl`**：串口出现 `HelloMuslDynOK`  
- `SYSCALLS.md` 含 mprotect/brk/arch_prctl 本阶段条目  
- `BUILD` 可复现获取/部署 musl 动态产物  
- NAS 构建脚本可复现，产物哈希记录在案

## 非目标
glibc；完整 Linux ld 行为；网络/图形（M7）；FAT 完整写；桌面；  
musl 的 `dlopen`/`dlsym`（M7+ 如需再补）。

## 顺序
1. NAS 构建 ld-musl-x86_64.so.1 + hello.musl，部署 ESP  
2. 内核跑 hello.musl，收集 ENOSYS 列表  
3. 逐个补齐 mmap/mprotect/brk/arch_prctl  
4. smoke-musl 通过 + 回归 + 文档

## 开工
从 NAS `paramiko` SSH 构建开始；内核端以真实 musl loader 的 strace 驱动。  
M6 自研 ld-helix smoke-dyn 不受影响。阶段末给 smoke-musl 验证命令与 serial 摘录。
