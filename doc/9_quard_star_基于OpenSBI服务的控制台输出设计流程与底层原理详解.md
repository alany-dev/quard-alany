# quard_star 基于 OpenSBI 服务的控制台输出设计流程与底层原理详解（基于当前改动）

## 1. 文档目标

本文针对你当前仓库里的最新改动，系统说明“基于 OpenSBI 服务实现控制台输出（`hello!`）”这条链路是如何设计、构建、启动并最终生效的。

文档覆盖两条主线：

1. 设计流程：从 `build.sh` 打包镜像，到 `boot/start.s` 装载，再到 OpenSBI 跳转并执行 `os.bin`。
2. 底层原理：从 S 模式 `ecall` 到 M 模式 OpenSBI 分发，再到 UART MMIO 和 QEMU 字符后端输出。

---

## 2. 当前改动基线（与本功能直接相关）

| 模块 | 文件 | 关键改动 | 作用 |
|---|---|---|---|
| 二阶段载荷 | `os/entry.S` `os/main.c` `os/sbi.c` `os/sbi.h` `os/os.ld` `os/Makefile` | 新增最小 S 模式程序：调用 `sbi_console_putchar` 输出 `hello!` | 验证通过 OpenSBI SBI 服务打印 |
| 镜像打包 | `build.sh` | 新增 `os` 编译与 `os.bin` 打包；`fw.bin` 增加 `seek=8K` 写入 | 把 `os.bin` 放入 pflash 固定偏移 |
| 低级引导 | `boot/start.s` | 新增 `os.bin` 拷贝：`0x20800000 -> 0x80200000` | 将二阶段程序搬运到 DRAM |
| OpenSBI domain 跳转 | `dts/quard_star_sbi.dts` | `untrusted-domain` 的 `next-addr` 从 `0x82000000` 改为 `0x80200000` | 让 boot hart 最终跳转到 `os.bin` |
| 运行脚本 | `run.sh` | 维持 `-bios none` + pflash 启动 | 使用你自定义启动链 |

---

## 3. 地址与镜像布局

## 3.1 `fw.bin` 内布局（打包结果）

来自 `build.sh` 的 `dd`：

1. `seek=0` 写 `lowlevel_fw.bin` -> 偏移 `0x000000`
2. `seek=512` 写 `quard_star_sbi.dtb` -> 偏移 `0x080000`
3. `seek=2K` 写 `fw_jump.bin` -> 偏移 `0x200000`
4. `seek=4K` 写 `trusted_fw.bin` -> 偏移 `0x400000`
5. `seek=8K` 写 `os.bin` -> 偏移 `0x800000`

QEMU pflash 基址是 `0x20000000`，因此 `os.bin` 在 Flash 的实际地址是：

- `0x20000000 + 0x800000 = 0x20800000`

## 3.2 lowlevel 运行后 DRAM 布局

`boot/start.s` 的搬运窗口：

1. OpenSBI：`[0x20200000, 0x20400000)` -> `[0x80000000, 0x80200000)`
2. DTB：`[0x20080000, 0x20100000)` -> `[0x82200000, 0x82280000)`
3. trusted：`[0x20400000, 0x20800000)` -> `[0xb0000000, 0xb0400000)`
4. OS：`[0x20800000, 0x20c00000)` -> `[0x80200000, 0x80600000)`

`os/os.ld` 链接基址是 `0x80200000`，与第 4 条完全对齐。

---

## 4. 完整设计流程（执行时序）

## 4.1 阶段 A：QEMU 复位向量跳转到 pflash

`qemu-8.0.2/hw/riscv/quard_star.c` 在 `quard_star_memory_create()` 中调用：

- `riscv_setup_rom_reset_vec(..., start_addr = 0x20000000, ...)`

含义：CPU 复位后先执行 MROM reset stub，再跳到 pflash 基址 `0x20000000` 执行 lowlevel。

## 4.2 阶段 B：lowlevel 搬运并跳 OpenSBI

`boot/start.s` 顺序：

1. 拷贝 OpenSBI 到 `0x80000000`
2. 拷贝 DTB 到 `0x82200000`
3. 拷贝 trusted 固件到 `0xb0000000`
4. 拷贝 `os.bin` 到 `0x80200000`
5. 设置 `a1 = 0x82200000`（传 FDT）
6. `jr 0x80000000`（进入 OpenSBI）

## 4.3 阶段 C：OpenSBI 冷启动并初始化控制台/ECALL

`opensbi-1.2/lib/sbi/sbi_init.c` 冷启动路径中依次做：

1. `sbi_console_init(scratch)`：初始化控制台设备
2. `sbi_ecall_init()`：注册 ecall 扩展（含 legacy 扩展）

你的平台 `platform_ops` 中 `console_init = fdt_serial_init`，意味着控制台设备由 DT `stdout-path` 自动选择。

## 4.4 阶段 D：Domain 覆盖下一跳地址（关键点）

你在 `dts/quard_star_sbi.dts` 里把 `untrusted-domain` 配成：

- `next-addr = 0x80200000`
- `next-mode = 0x1`（S-mode）
- `boot-hart = cpu0`

虽然 `FW_JUMP_ADDR=0x0`，但 OpenSBI 在 `sbi_domain_finalize()` 会对 cold hart 的 `scratch->next_*` 做覆盖：

- `scratch->next_addr = dom->next_addr`
- `scratch->next_mode = dom->next_mode`
- `scratch->next_arg1 = dom->next_arg1`

所以 boot hart 最终不是跳 `0x0`，而是跳到 `untrusted-domain` 的 `0x80200000`。

## 4.5 阶段 E：OpenSBI `mret` 到 S 模式执行 `os.bin`

`sbi_hart_switch_mode()` 做的事：

1. `MSTATUS.MPP` 设置为 `S`
2. `MEPC = 0x80200000`
3. `a0`/`a1` 设为传参
4. 执行 `mret`

之后 CPU 在 S 模式开始执行 `os/_start -> os_main`。

---

## 5. OS 侧 SBI 调用设计

## 5.1 最小调用路径

`os/main.c`：

1. 连续调用 `sbi_console_putchar('h') ... sbi_console_putchar('!')`

`os/sbi.c`：

1. `sbi_console_putchar()` 调 `sbi_ecall(SBI_EXT_0_1_CONSOLE_PUTCHAR, ...)`
2. `sbi_ecall()` 用内联汇编触发 `ecall`

## 5.2 寄存器 ABI 对应

当前实现严格按 SBI 调用约定传参：

1. `a7 = extid`（这里是 `0x1`）
2. `a6 = fid`（legacy 接口通常 `0`）
3. `a0..a5 = 参数`
4. `ecall` 返回后：`a0 = error`，`a1 = value`

对应关系在 `os/sbi.c` 中非常直接，便于后续扩展更多 SBI 服务。

---

## 6. OpenSBI 内部处理原理（ecall 到 putc）

## 6.1 Trap 分发

S 模式执行 `ecall` 后，硬件陷入 M 模式，OpenSBI 走：

1. `sbi_trap_handler()`：识别 `CAUSE_SUPERVISOR_ECALL`
2. `sbi_ecall_handler()`：读取 `regs->a7`/`regs->a6` 查扩展
3. 匹配到 legacy 扩展（`ecall_legacy`）

## 6.2 legacy console 处理

`sbi_ecall_legacy_handler()` 中：

1. `case SBI_EXT_0_1_CONSOLE_PUTCHAR: sbi_putc(regs->a0);`

所以 `a0` 里的字符被直接送入 OpenSBI 控制台层。

## 6.3 `sbi_putc()` 到具体串口驱动

`sbi_putc()` 依赖 `console_dev->console_putc`，这个 `console_dev` 在初始化时由 `sbi_console_set_device()` 设置。

`fdt_serial_init()` 逻辑：

1. 读取 `/chosen/stdout-path`
2. 找到对应节点（你的 DT 指向 `/soc/uart0@10000000`）
3. 依据 `compatible = "ns16550a"` 匹配 `fdt_serial_uart8250`
4. 调 `uart8250_init(...)` 注册 `uart8250_console`

最终输出函数落到 `uart8250_putc()`。

## 6.4 UART8250 写寄存器逻辑

`uart8250_putc()`：

1. 轮询 `LSR.THRE`
2. 写 `THR`（offset 0）发送字节

这一步是 OpenSBI 在 M 模式对 UART MMIO 的真实写操作。

---

## 7. QEMU 侧输出原理（MMIO 到宿主终端）

## 7.1 板级映射

`quard_star` 板级把 UART0 映射在 `0x10000000`，并在 `quard_star_serial_create()` 调 `serial_mm_init(...)` 建立 MMIO 设备。

## 7.2 写路径

QEMU 串口模型路径：

1. `serial_mm_write()`：MMIO 写入入口
2. `serial_ioport_write(addr=0)`：命中 THR
3. `serial_xmit()`
4. `qemu_chr_fe_write()`：把字符写到 chardev 后端

当运行参数把串口后端接到终端（如 `-nographic --serial mon:stdio`）时，字符就显示在宿主机控制台。

---

## 8. 特权级与上下文切换全链路

本功能实际经历三类切换：

1. M -> S：OpenSBI 最后一跳 `mret` 到 `os.bin`
2. S -> M：OS 通过 `ecall` 请求 SBI 服务
3. M -> S：OpenSBI 处理完成后恢复上下文并 `mret` 返回 OS

在 `output/qemu.log` 中可以直接看到这个证据链：

1. `Priv: 1` 且 `0x80200058: ecall`
2. 随后进入 `Priv: 3` 的 OpenSBI trap/dispatch 代码区
3. 再回到 `Priv: 1` 的 `0x8020005c` 继续执行

这证明你的实现不是“直接 MMIO 打印”，而是严格走了 SBI 服务调用。

---

## 9. 一句话结论

本次改动已经把 `quard_star` 的启动链从“OpenSBI 启动成功”推进到“二阶段 S 模式程序通过 SBI 服务完成控制台输出”，并且在代码、运行日志、特权级切换三层都可以自洽验证。

