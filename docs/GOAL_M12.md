# Goal: HelixOS M12 — cwd / chdir / 路径解析 / console stdin（历史）

> **状态：已完成（2026-08-01）** — **`HelixCwdOK`**。

## 范围

1. per-task `cwd[256]`；默认 `"/"`；fork 继承。
2. `getcwd`(79) / `chdir`(80)；目录存在性校验。
3. `vfs_path_resolve`：相对路径 + `.` / `..` / `//` → 绝对路径。
4. `open` / `mkdir` / `execve` / `newfstatat` 经 cwd。
5. console stdin：`cons_read` 轮询 COM1，空 → `-EAGAIN`。
6. msh：`cd` / `pwd`；`ls` 默认 `"."`。
7. helixbox：`chdir /tmp/cwdtest` + 相对 open → **`HelixCwdOK`**。

## 约束 / 非目标

- 不做完整 POSIX 权限 / symlink / mount 命名空间。
- FAT 子目录深度写仍受限（根 8.3 为主）。

## 验收

`make smoke-linux`：`HelixCwdOK` + `HelixMshOK` + `helixbox_smoke_done`。
