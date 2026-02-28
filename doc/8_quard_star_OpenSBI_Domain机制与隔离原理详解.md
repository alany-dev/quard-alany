# quard_star OpenSBI Domain 机制与隔离原理详解

> 适用对象：已经能读 OpenSBI/QEMU 源码，希望把“域隔离”从概念真正落到启动细节、地址空间和硬件约束的开发者。  
> 代码基线：当前仓库（`qemu-8.0.2/`、`opensbi-1.2/`、`boot/`、`dts/`、`trusted_domain/`）。  
> 文档目标：
> 1. 总结当前工程中与 Domain/Trusted Domain 相关的改动
> 2. 讲清 trusted_domain 从打包到运行的完整链路
> 3. 从 OpenSBI 底层实现解释 Domain 如何“真正生效”

---

## 1. 先看结论（Executive Summary）

当前工程已经形成一条完整的多域启动链：

1. QEMU `quard-star` 上电后从 MROM reset vector 跳到 pflash `0x20000000`。
2. 低级引导 (`boot/start.s`) 将 OpenSBI、DTB、trusted 固件搬运到 DRAM 指定位置。
3. OpenSBI 冷启动时创建 root domain，然后从 DT 的 `opensbi-domains` 解析并注册 `trusted-domain` 与 `untrusted-domain`。
4. OpenSBI 在 `sbi_domain_finalize()` 中为各域设置 boot hart：冷启动 hart0 进入 untrusted 域，hart7 被 HSM 启动进入 trusted 域。
5. `sbi_hart_pmp_configure()` 根据 domain memregion 给每个 hart 下发 PMP 规则，实现硬件级隔离。
6. `trusted_domain` 以 U-mode 运行，入口 `0xb0000000`，向 UART2 (`0x10002000`) 发送字符并驻留。

---

## 2. 现有代码改动总览（按模块）

> 这一节回答“当前项目都改了什么”。以仓库当前状态为准，重点列出 Domain/Trusted Domain 主链路。

| 模块 | 文件 | 关键改动 | 作用 |
|---|---|---|---|
| QEMU 板级 | `qemu-8.0.2/hw/riscv/quard_star.c` | 定义 quard-star 内存图（MROM/SRAM/CLINT/PLIC/UART/FLASH/DRAM）；在 `quard_star_memory_create()` 调用 `riscv_setup_rom_reset_vec()` 跳 pflash；创建 3 路 UART 映射 | 提供可控硬件平台和启动入口 |
| QEMU 板级头文件 | `qemu-8.0.2/include/hw/riscv/quard_star.h` | CPU 数、PLIC 参数、IRQ 编号等 | 板级常量定义 |
| 低级引导 | `boot/start.s` | 新增 trusted 固件拷贝：`0x20400000 -> 0xb0000000`，并保持 OpenSBI/DTB 搬运 | 在 OpenSBI 之前布置多域运行镜像 |
| 低级引导链接 | `boot/boot.lds` | lowlevel 固件链接到 pflash 起始 `0x20000000` | 让 MROM reset vector 跳转可执行 |
| OpenSBI 平台 | `opensbi-1.2/platform/quard_star/platform.c` | `domains_init = quard_star_domains_init -> fdt_domains_populate()`；`final_init` 调用 `fdt_domain_fixup()` | 开启 DT Domain 解析与向下游 DT 视图收敛 |
| OpenSBI 构建参数 | `opensbi-1.2/platform/quard_star/objects.mk` | `FW_JUMP=y`，`FW_TEXT_START=0x80000000`，`FW_JUMP_ADDR=0x0` | 固件形态/链接地址/下一跳策略 |
| Domain 描述 | `dts/quard_star_sbi.dts` | 新增 `/chosen/opensbi-domains`，定义 `trusted-domain`/`untrusted-domain`、region 和 `opensbi-domain` CPU 绑定 | 以 FDT 声明分区策略 |
| Trusted 载荷 | `trusted_domain/startup.s` + `trusted_domain/link.lds` | 新增 trusted 可执行镜像，链接入口 `0xb0000000`，向 UART2 输出 | trusted 域演示程序 |
| 构建脚本 | `build.sh` | 增加 trusted_domain 编译；`fw.bin` 增加 0x400000 偏移写入 trusted bin | 构建时完成三段镜像拼装 |
| 运行脚本 | `run.sh` | 使用多路 `--serial vc` + monitor 可视化窗口 | 便于观察多串口域输出 |

---

## 3. 地址空间全景

## 3.1 quard-star 物理地址映射（QEMU）

来源：`qemu-8.0.2/hw/riscv/quard_star.c:42-53`

| 资源 | 基址 | 大小 |
|---|---|---|
| MROM | `0x00000000` | `0x8000` |
| SRAM | `0x00008000` | `0x8000` |
| CLINT | `0x02000000` | `0x10000` |
| PLIC | `0x0c000000` | `0x210000` |
| UART0 | `0x10000000` | `0x100` |
| UART1 | `0x10001000` | `0x100` |
| UART2 | `0x10002000` | `0x100` |
| RTC | `0x10003000` | `0x1000` |
| FLASH(pflash) | `0x20000000` | `0x02000000` (32MB) |
| DRAM | `0x80000000` | `0x40000000` (1GB) |

## 3.2 `fw.bin` 内部布局（build 阶段）

来源：`build.sh:75-83`

| 组件 | `fw.bin` 偏移 | 运行物理地址（FLASH 基址 + 偏移） |
|---|---|---|
| lowlevel_fw.bin | `0x000000` | `0x20000000` |
| quard_star_sbi.dtb | `0x080000` | `0x20080000` |
| fw_jump.bin | `0x200000` | `0x20200000` |
| trusted_fw.bin | `0x400000` | `0x20400000` |

## 3.3 lowlevel 运行后 DRAM 布局

来源：`boot/start.s`

| 搬运内容 | 源地址 | 目的地址 | 大小 |
|---|---|---|---|
| OpenSBI fw_jump | `0x20200000` | `0x80000000` | `0x00200000` (2MB 窗口) |
| DTB | `0x20080000` | `0x82200000` | `0x00080000` (512KB 窗口) |
| trusted 固件 | `0x20400000` | `0xb0000000` | `0x00400000` (4MB 窗口) |

说明：trusted 实际 bin 文件远小于 4MB，但 lowlevel 按固定窗口复制，简化实现。

---

## 4. 启动全流程（从 reset 到两个 domain 启动）

## 4.1 阶段 A：QEMU 生成 reset vector（MROM）

`quard_star_memory_create()` 调 `riscv_setup_rom_reset_vec()`：
- `start_addr = QUARD_STAR_FLASH.base = 0x20000000`
- `rom_base = 0x0`
- `fdt_load_addr = 0x0`

对应：
- `qemu-8.0.2/hw/riscv/quard_star.c:126-131`
- `qemu-8.0.2/hw/riscv/boot.c:381-434`

reset vector 的核心行为：
1. `a0 <- mhartid`
2. `a1 <- fdt_load_addr`（本板级传 0）
3. `a2 <- fw_dynamic_info 指针`
4. `jr start_addr` 跳转到 `0x20000000`

## 4.2 阶段 B：执行 lowlevel 引导

lowlevel 代码（`boot/start.s`）做三次拷贝，然后跳转 OpenSBI：

1. 拷贝 OpenSBI 到 `0x80000000`
2. 拷贝 DTB 到 `0x82200000`
3. 拷贝 trusted 固件到 `0xb0000000`
4. `a1 = 0x82200000`，`jr 0x80000000`

这里 `a1` 是 OpenSBI 期望的 FDT 指针参数。

## 4.3 阶段 C：OpenSBI `_start` 建立 scratch 与 next_* 参数

来源：
- `opensbi-1.2/firmware/fw_jump.S:46-75`
- `opensbi-1.2/firmware/fw_base.S:296-315`

关键点：
1. `fw_next_arg1()` 默认返回输入 `a1`（即 `0x82200000`）
2. `fw_next_addr()` 从 `_jump_addr` 读，值来自 `FW_JUMP_ADDR`（当前为 `0x0`）
3. `fw_next_mode()` 固定返回 `PRV_S`
4. 这些值先写入每个 hart 的 scratch（`next_arg1/next_addr/next_mode`）

## 4.4 阶段 D：coldboot 初始化 Domain

调用顺序：
- `sbi_init()` -> `sbi_domain_init()` -> `sbi_domain_finalize()` -> `sbi_hart_pmp_configure()`

对应：
- `opensbi-1.2/lib/sbi/sbi_init.c:250`
- `opensbi-1.2/lib/sbi/sbi_init.c:317`
- `opensbi-1.2/lib/sbi/sbi_init.c:324`

这一步的因果关系非常关键：
- 先 finalize（确定每个 hart 属于哪个 domain，确定域 next_*）
- 再配置 PMP（物理内存保护）（按“最终域”下发硬件访问边界）

## 4.5 阶段 E：启动每个 domain 的 boot hart

`sbi_domain_finalize()` 遍历每个 domain，分别启动每个域的主核心（boot hart）：
- 如果是 cold hart（hart0），直接把 `scratch->next_*` 覆盖为该 domain next_*。
- 如果不是 cold hart（如 trusted 的 hart7），调用 `sbi_hsm_hart_start()` 拉起 hart7。
> cold hart : 系统上电 / 复位后第一个被唤醒、负责初始化整个系统的主核心（通常是 hart0）

对应：
- `opensbi-1.2/lib/sbi/sbi_domain.c:570-605`

结果：
- `untrusted-domain`：boot hart=0，下一跳 `0x82000000`，S-mode。
- `trusted-domain`：boot hart=7，下一跳 `0xb0000000`，U-mode。

---

## 5. Domain 在本工程里的具体定义

来源：`dts/quard_star_sbi.dts`

## 5.1 region 定义

1. `tmem`
- `base = 0xb0000000`
- `order = 28` -> 大小 `2^28 = 256MB`
- 覆盖区间：`[0xb0000000, 0xbfffffff]`

2. `tuart`
- `base = 0x10002000`
- `order = 8` -> 大小 `256B`
- 带 `mmio` 属性

3. `allmem`
- `base = 0x0`
- `order = 64` -> 全地址空间

## 5.2 domain 定义

1. `trusted-domain`
- `possible-harts = <cpu7>`
- `regions = <tmem 0x7>, <tuart 0x7>, <allmem 0x7>`
- `boot-hart = cpu7`
- `next-addr = 0xb0000000`
- `next-mode = 0`（U-mode）

2. `untrusted-domain`
- `possible-harts = <cpu0..cpu6>`
- `regions = <tmem 0x0>, <tuart 0x0>, <allmem 0x7>`
- `boot-hart = cpu0`
- `next-addr = 0x82000000`
- `next-mode = 1`（S-mode）

3. CPU 绑定
- `cpu0..cpu6` 的 `opensbi-domain = &udomain`
- `cpu7` 的 `opensbi-domain = &tdomain`

---

## 6. OpenSBI 底层原理：Domain 如何真正生效

这里按“策略层 -> 归一化层 -> 硬件层 -> SBI服务层”分层看。

## 6.1 策略层：FDT 解析 domain 描述

`fdt_domains_populate()` 解析 `/chosen/opensbi,domain,config`：
- 读 `possible-harts`
- 读 `regions` 中每个 memregion 的 `base/order/flags`
- 读 `boot-hart`、`next-arg1`、`next-addr`、`next-mode`
- 构造 `assign_mask`，调用 `sbi_domain_register()`

对应：
- `opensbi-1.2/lib/utils/fdt/fdt_domain.c:274-380`

关键补充：
OpenSBI 会把 root domain 中“无 RWX 的保护区”自动复制进每个子 domain，典型就是固件保护区和 M-mode only MMIO 保护区。

对应：`opensbi-1.2/lib/utils/fdt/fdt_domain.c:332-348`

## 6.2 归一化层：`sanitize_domain()`

每个 domain 注册前必须通过 `sanitize_domain()`：

1. 检查 hart mask 合法性
2. 检查 memregion 对齐与 `order` 合法性
3. 检查 domain 是否包含固件保护区
4. 检查 region 冲突并按优先级排序
5. 检查 `next_mode` 仅允许 S/U
6. 检查 `next_addr` 在该域内可执行

对应：`opensbi-1.2/lib/sbi/sbi_domain.c:201-307`

### 6.2.1 region 优先级为什么能“局部拒绝覆盖全局允许”

OpenSBI 对 region 的排序规则是：
- 小范围（小 order）优先
- 同 order 时 base 小的优先

对应：`is_region_before()`，`opensbi-1.2/lib/sbi/sbi_domain.c:187-199`

因此 `untrusted-domain` 里：
- `allmem (order=64, RWX)` 是总放行
- `tmem (order=28, flags=0)` 是更小范围拒绝
- `tuart (order=8, flags=0, mmio)` 是更小范围拒绝

最终命中顺序保证“拒绝洞”生效。

## 6.3 硬件层：PMP 下发

`init_coldboot()` 在 `sbi_domain_finalize()` 之后调用 `sbi_hart_pmp_configure()`。

对应：`opensbi-1.2/lib/sbi/sbi_init.c:317-325`

`sbi_hart_pmp_configure()` 会：
1. 取当前 hart 所属 domain
2. 遍历 domain regions
3. 将 region flags 映射到 PMP R/W/X/L
4. 调 `pmp_set()` 编程

对应：`opensbi-1.2/lib/sbi/sbi_hart.c:286-347`

这一步是“硬隔离”落地，不只是软件检查。

## 6.4 SBI 服务层：跨域请求再过滤

除了 PMP，OpenSBI 在关键 SBI 服务里还会做域检查：

1. HSM 启动/挂起
- `sbi_hsm_hart_start()`：目标 hart 必须属于当前域，目标地址必须当前域可执行
- 对应：`opensbi-1.2/lib/sbi/sbi_hsm.c:249-266`

2. IPI
- `sbi_ipi_send_many()` 先通过 `sbi_hsm_hart_interruptible_mask(dom, ...)` 过滤域内 hart
- 对应：`opensbi-1.2/lib/sbi/sbi_ipi.c:83-114`

3. 系统复位
- `sbi_system_reset()` 只有 `dom->system_reset_allowed` 才触发平台 reset
- 对应：`opensbi-1.2/lib/sbi/sbi_system.c:65-90`

这层是“接口级隔离”，防止 SBI 管理面跨域滥用。

---

## 7. trusted_domain 运行全流程细节

## 7.1 trusted 域镜像如何产生

`build.sh` 中新增：

1. 编译 `trusted_domain/startup.s`
2. 链接脚本 `trusted_domain/link.lds` 将入口放到 `0xb0000000`
3. 生成 `trusted_fw.bin`
4. 写入 `fw.bin` 偏移 `0x400000`

对应：`build.sh:56-65` 与 `build.sh:82-83`

## 7.2 trusted 镜像如何进入内存

lowlevel 固定执行：
- `0x20400000 -> 0xb0000000`
- 复制窗口 `4MB`

对应：`boot/start.s` 的第三段 `load_data`

## 7.3 trusted hart 如何被 OpenSBI 拉起

`sbi_domain_finalize()` 遍历到 `trusted-domain`：
- 其 boot hart 为 7，且不是 cold hart
- 调 `sbi_hsm_hart_start(..., hartid=7, saddr=0xb0000000, smode=U, priv=next_arg1)`

对应：
- `opensbi-1.2/lib/sbi/sbi_domain.c:588-598`
- `opensbi-1.2/lib/sbi/sbi_hsm.c:249-297`

## 7.4 trusted 代码实际在做什么

`trusted_domain/startup.s`：
1. 构造 UART2 基址 `0x10002000`
2. 连续向 THR 写入字符（每次写同一地址，属于 UART 正常发送方式）
3. 死循环驻留

UART2 映射来源：`qemu-8.0.2/hw/riscv/quard_star.c:256-262`

## 7.5 为什么 U-mode 还能访问 UART2

因为 domain + PMP 已显式允许：
- trusted 对 `tuart` 区域给了 `0x7`（R/W/X）
- `tuart` 节点带 `mmio`，且 base/order 覆盖 `0x10002000`

需要注意：
- 当前 `tuart order=8` 仅覆盖 `0x10002000-0x100020ff`（256B）
- 若 trusted 代码访问 UART2 更高偏移寄存器，可能触发访问失败

---

## 8. `domain_check` 与“命中第一条 region”机制

地址访问判定函数：`sbi_domain_check_addr()`。

对应：`opensbi-1.2/lib/sbi/sbi_domain.c:104-140`

行为总结：
1. 根据请求拼出期望权限（R/W/X/MMIO）
2. 按排序后的 region 顺序扫描
3. 命中第一条覆盖地址的 region 后立即判定允许/拒绝
4. S/U 未命中默认拒绝；M-mode 未命中默认放行

这也是为什么 region 排序规则是 Domain 语义的核心。

---

## 9. `fdt_domain_fixup()`：为什么会改写传给下一级的软件设备树

`quard_star_final_init()` 在 coldboot 调用 `fdt_domain_fixup(fdt)`。

对应：
- `opensbi-1.2/platform/quard_star/platform.c:89-103`
- `opensbi-1.2/lib/utils/fdt/fdt_domain.c:152-220`

它做三件事：
1. 删除 `/cpus/*` 上的 `opensbi-domain` 属性
2. 对当前域不允许的设备节点打 `status = "disabled"`
3. 删除 `/chosen/opensbi,domain,config` 节点

作用：
- 下一级软件只看到自己可见的硬件视图
- 隐藏 M-mode 域策略内部细节

---

## 10. 历史报错复盘：`sanitize_domain ... conflict`

你遇到过的典型报错：

```text
sanitize_domain: untrusted-domain conflict between regions ...
sbi_domain_register: sanity checks failed for untrusted-domain (error -3)
sbi_domain_finalize: platform domains_init() failed (error -3)
init_coldboot: domain finalize failed (error -3)
```

根因模型：
- `sanitize_domain()` 判定两个 region “子集关系 + flags 完全相同”会冲突。
- 若 `untrusted` 手工拒绝区（如 `tmem`）覆盖到 OpenSBI 自动加入的“固件保护区”，且 flags 同为 `0x0`，会触发冲突。

对应判定代码：`opensbi-1.2/lib/sbi/sbi_domain.c:260-267`

### 10.1 为什么当前构建通常不再触发

当前 `FW_TEXT_START=0x80000000`，固件保护区自动落在 `0x80000000` 一带；而 `tmem` 在 `0xb0000000`。两者不重叠，自然无冲突。

---

## 11. 本文对应的关键源码索引

- QEMU 板级入口与内存图：
  - `qemu-8.0.2/hw/riscv/quard_star.c`
  - `qemu-8.0.2/include/hw/riscv/quard_star.h`

- MROM reset vector：
  - `qemu-8.0.2/hw/riscv/boot.c:riscv_setup_rom_reset_vec()`

- lowlevel 与镜像拼装：
  - `boot/start.s`
  - `boot/boot.lds`
  - `build.sh`

- OpenSBI 平台接入：
  - `opensbi-1.2/platform/quard_star/platform.c`
  - `opensbi-1.2/platform/quard_star/objects.mk`

- Domain 核心：
  - `opensbi-1.2/lib/sbi/sbi_domain.c`
  - `opensbi-1.2/lib/utils/fdt/fdt_domain.c`
  - `opensbi-1.2/lib/sbi/sbi_hart.c`
  - `opensbi-1.2/lib/sbi/sbi_hsm.c`
  - `opensbi-1.2/lib/sbi/sbi_ipi.c`
  - `opensbi-1.2/lib/sbi/sbi_system.c`

- Domain 策略来源：
  - `dts/quard_star_sbi.dts`

- Trusted 载荷：
  - `trusted_domain/startup.s`
  - `trusted_domain/link.lds`

---

## 12. 结论

在当前项目中，OpenSBI Domain 不是“配置文件层面的逻辑隔离”，而是完整的端到端机制：

1. FDT 声明策略
2. OpenSBI 归一化与校验
3. PMP 下发形成硬件边界
4. SBI 管理面做域内过滤
5. 最终由各域 boot hart 落到不同入口执行（hart0/untrusted，hart7/trusted）

这条链路打通后，你后续接入 RTOS/Linux/TEE 时，只需要围绕 `next_addr/next_mode/region` 扩展，不需要重做启动底座。
