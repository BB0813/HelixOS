# Goal: D7 — DX 痛点 + FAT stat 真实化

## Context

D6 后继续收尾两个真实差距：`mkesp.sh` 静默跳过缺失 user 二进制（调试陷阱根因），
`fstat` 返回假时间戳（1970-01-01）。

## D7.1 — mkesp.sh 硬失败

**背景**：`git clean -fd build/` 删掉 helixbox.elf 后，`make -j2` 只重建 kernel
不重建 user；直接 `bash scripts/run-qemu.sh` 绕过 `make esp` 的 user 依赖 →
ESP FAT image 缺 helixbox → busybox chain 正常跑但 helixbox exec 静默失败 →
假 "musl hang"（D4 调试浪费 30 分钟根因）。

**改动**（`scripts/mkesp.sh`）：init.elf / task2.elf / helixbox.elf / msh.elf /
tui.elf 任一缺失时 `echo "error: build/user/X.elf missing — run 'make user' first" >&2; exit 1`。

**验收**：`rm build/user/*.elf && bash scripts/mkesp.sh` → 非零退出 + 明确报错。

## D7.2 — FAT stat 真实字段 + CMOS RTC

### RTC（`kernel/arch/x86_64/timer.c`）

```c
u64 rtc_unix_seconds(void);
```

- CMOS port 0x70（index）/ 0x71（data）；reg 0x00/0x02/0x04/0x07/0x08/0x09 =
  sec/min/hour/day/month/year（BCD）
- status A (0x0A) bit 7 = update-in-progress → 轮询等 clear
- status B (0x0B) bit 2 set = binary mode（假设 BCD，非 set 时 bcd_to_bin）
- 12/24h 转换（status B bit 1）
- year = 2000 + 2-digit yr；Howard Hinnant civil→unix 换算

### FAT dirent 时间戳（`kernel/fs/fat.c`）

FAT u16 date/time 布局（LE）：`year-1980<<9 | month<<5 | day`，
`hours<<11 | minutes<<5 | sec/2`。dirent offset 14/16/18/22/24 =
crt_time/crt_date/acc_date/wrt_time/wrt_date。

- `struct fat_file` 加 5 字段：wrt_time/wrt_date/acc_date/crt_time/crt_date
- `struct fat_dirent_meta { u32 clus; u32 size; u8 attr; u16 wrt_time, wrt_date,
  acc_date, crt_time, crt_date; }`
- `find_in_dir()` 填 meta；`fat_resolve()` 加 `out_meta`（3 caller 更新）
- `fat_date_to_unix()` / `fat_unix_to_date()`（Hinnant inverse）
- `fill_83_dirent()` 用 `rtc_unix_seconds()` stamp 新建文件
- `fat_fstat()` 填 `st_ino = start_clus` + `st_mtime = fat_date_to_unix(wrt_date,
  wrt_time)` + `st_atime`（acc_date）+ `st_ctime`（crt_date/crt_time）

### Smoke（`user/helixbox.c`）

`HelixStatOK`：open `/HELIXW.TXT` → `usys(SYS_fstat, fd, stbuf, 0)`（144 字节
struct，st_ino@8 / st_size@48 / st_mtime@88）→ 验证 `st_size==16 && st_ino!=0 &&
st_mtime!=0`。

## 验收

- `make smoke-linux` 含 `HelixStatOK`，无 FAIL
- `ls -l /bin/helixbox` 显示真实时间戳（非 1970-01-01）
- `make smoke-fs` EXIT=0 不回归

## 串口 marker

| Marker | 含义 |
|--------|------|
| **HelixStatOK** | fstat 返回真实 st_ino + st_size + st_mtime |
