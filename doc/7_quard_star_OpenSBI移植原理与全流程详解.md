# quard_star OpenSBI 移植原理与全流程详解（面向初学者）

> 适用对象：刚接触 RISC-V 启动链、OpenSBI 和 QEMU 板级建模的开发者。  
> 分析基线：当前仓库代码（`quard_star` + `opensbi-1.2`）和 2026-02-13 可验证的官方资料。  
> 结论先行：你已经成功把 OpenSBI 跑起来了，当前还缺“OpenSBI 跳转到下一级软件”的最后一跳配置。
> 版本核对：截至 2026-02-13，OpenSBI 官方 release 页面显示最新版本为 `v1.8.1`（发布日期 2026-01-08）；你当前工程使用的是 `opensbi-1.2`。

---

## 1. OpenSBI 是什么，为什么需要它

### 1.1 先记住这句话
在 RISC-V 里，**S 模式操作系统（比如 Linux）不能直接做所有硬件控制**，很多机器级能力要通过 SBI 接口请求 M 模式固件执行。  
OpenSBI 就是这个 SBI 的开源参考实现。

OpenSBI 官方 README 明确了 SBI 的定位：  
- M-mode 固件 <-> S/HS-mode 软件的标准接口  
- OpenSBI 提供 `libsbi.a`（平台无关）+ `libplatsbi.a`（平台相关）组合来实现该接口

参考：  
- https://github.com/riscv-software-src/opensbi  
- https://raw.githubusercontent.com/riscv-software-src/opensbi/master/README.md

### 1.2 SBI 调用是怎么发起的
SBI 通过 `ecall` 触发。调用约定（来自 SBI 规范）是：

- `a7`：Extension ID（扩展号）
- `a6`：Function ID（函数号）
- `a0` 到 `a5`：参数
- 返回时：
  - `a0`：error code
  - `a1`：return value

参考：  
- https://raw.githubusercontent.com/riscv-non-isa/riscv-sbi-doc/master/src/binary_encoding.adoc

### 1.3 OpenSBI 的代码分层（你读源码时最重要的思维模型）

1. `libsbi.a`  
职责：SBI 核心逻辑（trap/ecall、HSM、定时器框架、域管理、切换到下一阶段等）。

2. `libplatsbi.a`  
职责：把平台相关实现（串口、中断、IPI、timer、域）“挂钩”给 `libsbi`。  
你移植 `quard_star` 主要改的就是这一层。

3. firmware（`fw_jump` / `fw_dynamic` / `fw_payload`）  
职责：启动时把参数整理好，初始化 OpenSBI 运行时，然后跳去下一阶段。

---

## 2. 你的工程选择了哪种固件形态

你在 `opensbi-1.2/platform/quard_star/objects.mk` 里配置了：

- `FW_JUMP=y`
- `FW_TEXT_START=0xBFF80000`
- `FW_JUMP_ADDR=0x0`

对应文件：`opensbi-1.2/platform/quard_star/objects.mk:15`  
这表示：

1. 使用 `fw_jump` 形态  
2. OpenSBI 链接地址设为 `0xBFF80000`  
3. OpenSBI 初始化后，下一阶段入口地址是 `0x0`（这是当前关键限制）

### 2.1 `fw_jump` 和 `fw_dynamic` 的本质差异

`fw_jump`（你当前使用）：
- 下一跳地址主要由编译期参数（`FW_JUMP_ADDR`）给定。
- 启动阶段不必构造 `fw_dynamic_info`。
- 简单直接，但灵活性低。

`fw_dynamic`：
- 上一阶段在运行时构造 `struct fw_dynamic_info`，并把其地址放在 `a2` 传给 OpenSBI。
- 下一跳地址、模式、选项都可运行时决定。
- 更灵活，常见于 U-Boot SPL/BootROM 组合。

官方文档：  
- https://raw.githubusercontent.com/riscv-software-src/opensbi/master/docs/firmware/fw_jump.md  
- https://raw.githubusercontent.com/riscv-software-src/opensbi/master/docs/firmware/fw_dynamic.md

---

## 3. 你的镜像打包与地址布局（非常关键）

你在 `build.sh` 做了三段拼装（`output/fw/fw.bin`）：

1. offset `0x000000` 写入 `lowlevel_fw.bin`
2. offset `0x080000` 写入 `quard_star_sbi.dtb`
3. offset `0x200000` 写入 `fw_jump.bin`

对应命令：`build.sh:74`、`build.sh:76`、`build.sh:78`

### 3.1 与 QEMU Flash 地址合并后

`quard_star` 的 pflash 基址是 `0x20000000`（`qemu-8.0.2/hw/riscv/quard_star.c:51`），所以三段实际物理地址是：

- `lowlevel_fw.bin`：`0x20000000 + 0x000000 = 0x20000000`
- `quard_star_sbi.dtb`：`0x20000000 + 0x080000 = 0x20080000`
- `fw_jump.bin`：`0x20000000 + 0x200000 = 0x20200000`

你在 `boot/start.s` 的拷贝代码正好按这个映射写死了地址：

- `0x20200000 -> 0x80000000`（拷贝 OpenSBI）
- `0x20080000 -> 0x82200000`（拷贝 DTB）

对应文件：`boot/start.s:29` 到 `boot/start.s:58`

---

## 4. 全流程时序：从上电到 OpenSBI，再到下一跳

下面按真实执行顺序讲。

### 阶段 A：QEMU 板级初始化

入口：`qemu-8.0.2/hw/riscv/quard_star.c:275`

`quard_star_machine_init()` 依次创建：
- CPU (`quard_star_cpu_create`)
- 内存与 MROM (`quard_star_memory_create`)
- pflash (`quard_star_flash_create`)
- PLIC/ACLINT
- UART/RTC

### 阶段 B：QEMU 在 MROM 放 reset vector

在 `quard_star_memory_create()` 中调用：

`riscv_setup_rom_reset_vec(machine, ..., start_addr=0x20000000, rom_base=0x0, ..., kernel_entry=0, fdt_load_addr=0)`

对应：
- `qemu-8.0.2/hw/riscv/quard_star.c:126`
- `qemu-8.0.2/hw/riscv/boot.c:381`

这一段 reset stub 会：
- `a0 <- mhartid`
- `a1 <- fdt_load_addr`（这里是 0）
- `a2 <- fw_dynamic_info 地址`（MROM 内部）
- `jr start_addr`（这里跳 `0x20000000`）

### 阶段 C：进入你的 lowlevel 引导（pflash 起始）

执行 `boot/start.s`：

1. 拷贝 OpenSBI 二进制到 DRAM `0x80000000`
2. 拷贝 DTB 到 DRAM `0x82200000`
3. 设置 `a1=0x82200000`（传给 OpenSBI 当 FDT（DTB 是 FDT 的二进制文件形态，FDT 是这套硬件描述体系的通用名称））
4. 跳转到 `0x80000000`

关键行：`boot/start.s:31` 到 `boot/start.s:58`

### 阶段 D：OpenSBI 入口与重定位

`fw_jump` 入口在 `fw_base.S` 的 `_start`。  
你虽然把它“加载到 `0x80000000`”，但它会按链接地址与重定位逻辑运行，最终在 `0xBFF8xxxx` 区间执行（和 `FW_TEXT_START=0xBFF80000` 对齐）。

> 你 lowlevel 引导程序把 fw_jump.bin 从 Flash 拷贝到 DRAM 0x80000000，但 OpenSBI 编译时链接到 0xBFF80000—— 这时候 OpenSBI 会自动做重定位（Relocation），把自身代码从 0x80000000 复制到 0xBFF80000 后再执行，确保和链接地址一致。

关键文件：
- `opensbi-1.2/firmware/fw_base.S:49`（`_start`）
- `opensbi-1.2/firmware/fw_base.S:123`（非 PIC 重定位路径）
- `opensbi-1.2/platform/quard_star/objects.mk:16`（`FW_TEXT_START`）

实际运行也验证了这一点：OpenSBI 启动信息显示

- `Firmware Base : 0xbff80000`

### 阶段 E：平台初始化（你移植最核心部分）

`fw_platform_init()` 在 `platform.c` 中做两件事：

1. 从 FDT 读 model，写入平台名  
2. 扫描 `/cpus`，建立 `hart_index -> hartid` 映射并设置 `hart_count`

对应：`opensbi-1.2/platform/quard_star/platform.c:40` 到 `opensbi-1.2/platform/quard_star/platform.c:81`

随后 `platform_ops` 里把平台能力接到通用框架：
- `console_init = fdt_serial_init`
- `irqchip_init = fdt_irqchip_init`
- `ipi_init = fdt_ipi_init`
- `timer_init = fdt_timer_init`
- `domains_init = fdt_domains_populate`
- `pmu_init = fdt_pmu_setup`

对应：`opensbi-1.2/platform/quard_star/platform.c:151` 到 `opensbi-1.2/platform/quard_star/platform.c:169`

### 阶段 F：OpenSBI 初始化完成后切换到下一阶段

`sbi_init()` 最终调用：

`sbi_hart_switch_mode(..., scratch->next_addr, scratch->next_mode, ...)`

对应：`opensbi-1.2/lib/sbi/sbi_init.c:354`、`opensbi-1.2/lib/sbi/sbi_hart.c:754`

`fw_jump` 的 `fw_next_addr()` 读 `_jump_addr`，而 `_jump_addr` 来自 `FW_JUMP_ADDR`：

- `fw_next_addr()`：`opensbi-1.2/firmware/fw_jump.S:61`
- `_jump_addr` 定义：`opensbi-1.2/firmware/fw_jump.S:95`
- 你配置为 0：`opensbi-1.2/platform/quard_star/objects.mk:17`

所以 OpenSBI 最后会把 `mepc` 设为 `0`，`mret` 到 S-mode 地址 `0x0`。

---

## 5. 你当前运行结果怎么解读（基于实际输出）

短时运行输出（已验证）显示：

- OpenSBI 启动成功，版本 `v1.2`
- 平台识别成功：`Platform Name : riscv-quard-star,qemu`
- 8 个 HART 被识别
- `Domain0 Next Address : 0x0000000000000000`
- `Domain0 Next Arg1 : 0x0000000082200000`

这说明你的移植主线已打通：

1. QEMU 板级建模成功  
2. lowlevel 装载成功  
3. OpenSBI 平台端口成功  
4. SBI runtime 启动成功

但当前“下一阶段镜像”没有接上：  
`Next Address = 0` 会在切到 S-mode 后异常，再回到 OpenSBI trap 处理路径，形成异常循环。

---

## 6. 涉及代码与文件逐个解释

## 6.1 构建与运行脚本层

`build.sh`
- 负责编译 QEMU、lowlevel、OpenSBI，并把三段固件拼进一个 pflash 镜像。
- OpenSBI 编译入口：`make CROSS_COMPILE=... PLATFORM=quard_star`（`build.sh:56`）。
- 拼装策略在 `build.sh:74` 到 `build.sh:78`。

`run.sh`
- 固定用 `-M quard-star -bios none -drive if=pflash...` 启动（`run.sh:7` 到 `run.sh:11`）。
- `-bios none` 的含义是：不走 QEMU 默认 BIOS 路径，控制权交给你的 MROM reset vector + pflash 内容。

## 6.2 lowlevel 引导层

`boot/boot.lds`
- 把 lowlevel 链接到 Flash 地址空间 `0x20000000`，大小窗口 `512K`。  
对应：`boot/boot.lds:8`

`boot/start.s`
- 你自定义的“前级加载器”。
- 核心职责：搬运 OpenSBI 和 DTB，设置寄存器，跳转。
- 参数语义：
  - 跳 OpenSBI 前 `a0=mhartid`（保留）
  - `a1=0x82200000`（DTB 地址）
  - `a2` 在 `fw_jump` 场景不参与关键决策

## 6.3 QEMU 板级层

`qemu-8.0.2/include/hw/riscv/quard_star.h`
- 定义 machine 状态结构、memmap 索引、中断号、PLIC 参数。  
- 你当前最大 CPU 数是 8（`quard_star.h:9`）。

`qemu-8.0.2/hw/riscv/quard_star.c`
- 定义物理地址映射（MROM/CLINT/PLIC/UART/FLASH/DRAM）。
- `quard_star_memory_create()` 里调用 `riscv_setup_rom_reset_vec()` 生成 reset stub。
- `quard_star_flash_create()` 把 `-drive if=pflash` 文件挂到 `0x20000000`。
- DRAM 改成 `0x40000000`（1GB）后，OpenSBI 和后续镜像有足够地址空间。

`qemu-8.0.2/hw/riscv/boot.c`
- `riscv_setup_rom_reset_vec()` 是 reset 入口“机器码模板工厂”。
- 负责在 MROM 写入 reset 指令和 fw_dynamic_info（即使你现在跑 fw_jump）。

## 6.4 OpenSBI 平台端口层（你新增的核心）

`opensbi-1.2/platform/quard_star/Kconfig`
- 声明平台开关并选择 FDT/domain/PMU 基础能力。

`opensbi-1.2/platform/quard_star/configs/defconfig`
- 当前内容基本跟 generic defconfig 一样，启用了常见 FDT 驱动。

`opensbi-1.2/platform/quard_star/objects.mk`
- 决定构建 `platform.o` 和 `FW_JUMP` 参数。
- `FW_TEXT_START` 与 `FW_JUMP_ADDR` 都在这里控制。

`opensbi-1.2/platform/quard_star/platform.c`
- `fw_platform_init()`：从 FDT 提取平台信息和 hart 拓扑。
- `platform_ops`：连接串口、中断、IPI、timer、domain、pmu。
- `platform` 结构体：把平台元数据交给 `libsbi`。

## 6.5 OpenSBI 固件内核层（建议重点读）

`opensbi-1.2/firmware/fw_base.S`
- 所有固件类型的公共启动主干（重定位、scratch 初始化、trap 基础设施、调 `sbi_init`）。

`opensbi-1.2/firmware/fw_jump.S`
- 定义 `fw_next_addr()`、`fw_next_arg1()`、`fw_next_mode()` 等“下一跳策略”。
- 你的 `FW_JUMP_ADDR` 就是在这里被取出来作为下一跳地址。

`opensbi-1.2/lib/sbi/sbi_init.c`
- OpenSBI runtime 冷启动/热启动主流程（init 各子系统、打印信息、切换模式）。

`opensbi-1.2/lib/sbi/sbi_hart.c`
- `sbi_hart_switch_mode()` 最终写 `mstatus/mepc` 然后 `mret` 到下一阶段。

---

## 7. 当前实现里的关键一致性点与潜在坑

### 7.1 已经对齐、且运行验证通过的点

1. Flash 地址与 lowlevel 拷贝地址对齐  
2. lowlevel 传给 OpenSBI 的 DTB 地址有效  
3. OpenSBI 平台能从 DTB 枚举 8 HART  
4. OpenSBI 控制台（uart8250）可正常输出启动信息

### 7.2 建议你尽快确认的点

1. `FW_JUMP_ADDR=0x0` 仅适合“验证 OpenSBI 能跑起来”，不适合真实启动链。  
2. `build.sh` 中 `seek=2k` 的注释地址写错（实际是 `0x200000`）。  
3. `dts/quard_star_sbi.dts` 里 `uart1/uart2` 的 `interrupts` 仍是 `0xa`，但 QEMU 板级代码里分别接了 11/12。  
4. `run.sh` 的 pflash 默认可写，若镜像文件权限只读会报 `Permission denied`。

---

## 8. 你下一步该怎么把“最后一跳”补齐

下面给你两条可落地路径。

### 路线 A：继续用 `fw_jump`（改动最小）

1. 选定下一阶段镜像入口地址（例如 U-Boot 或 Linux Image 的加载地址）。  
2. 把 `FW_JUMP_ADDR` 改为该地址。  
3. 在 lowlevel 里把下一阶段镜像拷贝到对应地址。  
4. 如需固定 DTB 落点，可配 `FW_JUMP_FDT_ADDR`。

优点：简单、直观。  
缺点：灵活性较弱，地址变化要重新构建。

### 路线 B：切到 `fw_dynamic`（更标准）

1. 在 `objects.mk` 启用 `FW_DYNAMIC=y`。  
2. lowlevel 在内存构造 `struct fw_dynamic_info`，把地址放进 `a2`。  
3. `next_addr/next_mode/options` 运行时可变。  

优点：更贴近主流 boot chain（SPL/FSBL -> OpenSBI）。  
缺点：前级代码会比 `fw_jump` 略复杂。

---

## 9. 小白视角总结（你现在到底做成了什么）

你这次移植不是“只改了个编译选项”，而是完整打通了这条链：

1. 自定义 QEMU 板级硬件模型（quard_star）  
2. 让 QEMU reset 向量跳到你控制的 pflash 入口  
3. 自己实现前级加载器，把 OpenSBI 和 DTB 搬到目标内存  
4. 增加 OpenSBI 平台端口，让它知道 quard_star 的 CPU/中断/串口/定时器  
5. 实际看到 OpenSBI 启动横幅和平台信息

对初学者来说，这已经是“OpenSBI 移植”里最难的一大半。  
剩下的核心就是把 `next_addr` 指向真正的下一阶段镜像。

---

## 10. 官方参考（建议收藏）

1. OpenSBI 项目主页  
https://github.com/riscv-software-src/opensbi

2. OpenSBI README（项目目标、分层架构）  
https://raw.githubusercontent.com/riscv-software-src/opensbi/master/README.md

3. FW_JUMP 官方文档  
https://raw.githubusercontent.com/riscv-software-src/opensbi/master/docs/firmware/fw_jump.md

4. FW_DYNAMIC 官方文档  
https://raw.githubusercontent.com/riscv-software-src/opensbi/master/docs/firmware/fw_dynamic.md

5. OpenSBI release 页面（最新版本核对）  
https://github.com/riscv-software-src/opensbi/releases

6. SBI 规范（调用约定与扩展）  
https://github.com/riscv-non-isa/riscv-sbi-doc  
https://raw.githubusercontent.com/riscv-non-isa/riscv-sbi-doc/master/src/binary_encoding.adoc

---

## 附：本次运行观察到的实机输出关键字段

- `OpenSBI v1.2`
- `Platform Name : riscv-quard-star,qemu`
- `Platform HART Count : 8`
- `Firmware Base : 0xbff80000`
- `Domain0 Next Address : 0x0000000000000000`
- `Domain0 Next Arg1 : 0x0000000082200000`
- `Domain0 Next Mode : S-mode`

这组字段与本仓库代码完全对应，能作为你后续改 `FW_JUMP_ADDR` 或切 `FW_DYNAMIC` 的回归基线。
