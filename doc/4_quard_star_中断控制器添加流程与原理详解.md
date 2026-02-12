# quard_star 中断控制器添加全流程与原理详解
> 适读人群：刚接触 QEMU/RISC-V 板级建模的新手。  

---

## 0. 执行摘要（先看结论）

你这次提交，已经完成了 **中断子系统的基础骨架**：

1. 在 `QUARD_STAR` 机型中启用了 `ACLINT` 与 `PLIC` 相关构建能力。
2. 在板级内存映射里加入了 `CLINT/ACLINT` 和 `PLIC` 的地址窗口。
3. 在 machine init 流程里，新增了：
   - `quard_star_plic_create()`：创建平台级外部中断控制器（PLIC）
   - `quard_star_aclint_create()`：创建本地软件中断 + 定时器中断（ACLINT）
4. PLIC/ACLINT 都完成了到 CPU 中断输入线的连接。

一句话总结：

**你已经把“中断高速公路主干道”修好了（PLIC + ACLINT -> CPU），但“外设匝道”还需要逐个接入（比如 UART IRQ 接到 PLIC 输入脚）。**

---

## 1. 本次改动清单

### 1.1 构建开关层

文件：`qemu-8.0.2/hw/riscv/Kconfig`

在 `config QUARD_STAR` 下新增：

- `select RISCV_ACLINT`
- `select RISCV_APLIC`
- `select SIFIVE_PLIC`

说明：

- `RISCV_ACLINT`：提供本地中断设备实现（MSWI/MTIMER）。
- `SIFIVE_PLIC`：提供平台级外部中断控制器实现。
- `RISCV_APLIC`：当前代码里尚未使用（属于预留能力）。

---

### 1.2 板级状态与常量层

文件：`qemu-8.0.2/include/hw/riscv/quard_star.h`

新增核心内容：

1. `DeviceState *plic[QUARD_STAR_SOCKETS_MAX];`
2. `enum` 中新增 `QUARD_STAR_CLINT`、`QUARD_STAR_PLIC`
3. 一组 PLIC 参数宏（source 数量、priority、pending/enable/context 偏移等）

这些宏本质是在定义：

- PLIC 的“寄存器地图”
- PLIC 支持多少路中断输入
- 每个 hart context 的寄存器步长

---

### 1.3 板级初始化层

文件：`qemu-8.0.2/hw/riscv/quard_star.c`

关键新增：

1. 头文件引入 `#include "hw/intc/sifive_plic.h"`
2. `quard_star_memmap[]` 增加：
   - `QUARD_STAR_CLINT = 0x02000000, 0x10000`
   - `QUARD_STAR_PLIC  = 0x0c000000, 0x4000000`
3. 新增函数：
   - `quard_star_plic_create()`
   - `quard_star_aclint_create()`
4. 在 `quard_star_machine_init()` 中调用上面两个函数

---

## 2. 先建立基础概念：RISC-V 中断体系到底分几层？

在你这份实现里，中断分两大类：

1. **本地中断（Local Interrupt）**  
   每个 hart 独有，主要包括：
   - 软件中断（MSIP）
   - 定时器中断（MTIP）
   由 `ACLINT` 负责。

2. **平台外部中断（Platform External Interrupt）**  
   来自 UART/GPIO/PCIe/virtio 等外设，先进入 PLIC，再由 PLIC 分发到目标 hart。  
   由 `PLIC` 负责。

你可以把它想成：

- ACLINT = “每个核自己的闹钟 + 对讲机”
- PLIC = “整块板子的总机分线台”

---

## 3. QEMU 视角下，设备是如何“活”起来的？

不管 ACLINT 还是 PLIC，典型生命周期都是：

1. `qdev_new(TYPE_XXX)`：创建设备对象
2. `qdev_prop_set_*`：写入参数
3. `sysbus_realize_and_unref(...)`：设备上电（realize）
4. `sysbus_mmio_map(...)` 或 `memory_region_add_subregion(...)`：挂到物理地址空间
5. `qdev_connect_gpio_out(...)`：把设备 IRQ 输出线接到 CPU 或其他控制器

你的新增代码正是沿着这条标准路径在做。

---

## 4. 从 machine init 开始的完整执行流程

入口函数：`quard_star_machine_init()`

执行顺序（你当前代码）是：

1. `quard_star_cpu_create(machine)`
2. `quard_star_memory_create(machine)`
3. `quard_star_flash_create(machine)`
4. `quard_star_plic_create(machine)`
5. `quard_star_aclint_create(machine)`

下面重点讲 4 和 5。

---

## 5. PLIC 创建流程与原理（外部中断主干）

函数：`quard_star_plic_create(MachineState *machine)`

### 5.1 per-socket 循环

代码按 socket 循环处理：

- 取 `hart_count`
- 取 `base_hartid`
- 生成 `plic_hart_config`
- 调 `sifive_plic_create(...)`

这说明你已经在架构上考虑了 NUMA/socket 维度（即每个 socket 可拥有独立 PLIC 实例）。

---

### 5.2 `plic_hart_config` 在干嘛？

调用来源：`riscv_plic_hart_config_string(...)`（`hw/riscv/boot.c`）

这个字符串用来描述“每个 hart 有哪些 context（M/S）”。典型值：

- `"M"`
- `"MS,MS"`

PLIC realize 时会解析该字符串（`parse_hart_config()`），得到：

- `num_addrs`（context 数）
- 每个 context 对应 `hartid + mode`

这是 PLIC 后续 `enable/context` 寄存器寻址、以及输出线路由的基础。

---

### 5.3 `sifive_plic_create(...)` 做了什么？

它内部做了三件核心事：

1. **配置属性 + realize + mmio 映射**
2. **创建 GPIO 输入脚**（外设向 PLIC 报中断）
3. **把 PLIC 输出脚接到 CPU**（M_EXT/S_EXT）

#### 5.3.1 输入脚

`qdev_init_gpio_in(dev, sifive_plic_irq_request, s->num_sources);`

意义：

- PLIC 对外暴露 `num_sources` 路输入
- 外设把 IRQ 接到这些输入脚之一

#### 5.3.2 输出脚

`qdev_init_gpio_out(dev, s->s_external_irqs, s->num_harts);`  
`qdev_init_gpio_out(dev, s->m_external_irqs, s->num_harts);`

然后在 create 尾部，把每个输出脚连到 CPU：

- `IRQ_S_EXT`
- `IRQ_M_EXT`

这就是“PLIC 把候选中断送到哪个 hart 的哪条中断线”的硬连接。

---

### 5.4 PLIC 运行时核心算法

#### 5.4.1 外设触发

外设拉高中断输入 -> `sifive_plic_irq_request()` -> 设置 pending 位 -> `sifive_plic_update()`。

#### 5.4.2 仲裁

`sifive_plic_claimed()` 会在：

`pending & enable & ~claimed`

中找“优先级最高且高于阈值”的 IRQ。

#### 5.4.3 拉高中断线

`sifive_plic_update()` 根据 context 的 mode（M/S）决定拉高：

- `m_external_irqs[...]`
- 或 `s_external_irqs[...]`

#### 5.4.4 claim/complete 协议

Guest 访问 context 区域：

- 读 `context + 4`：claim（取到 IRQ ID，并从 pending 转 claimed）
- 写 `context + 4`：complete（清 claimed，允许再次投递）

这就是 PLIC 标准的“取号-办事-销号”闭环。

---

## 6. ACLINT 创建流程与原理（本地中断）

函数：`quard_star_aclint_create(MachineState *machine)`

你每个 socket 创建了两个设备：

1. `riscv_aclint_swi_create(...)`（MSWI）
2. `riscv_aclint_mtimer_create(...)`（MTIMER）

且地址布局采用：

- `SWI` 放在 CLINT 基址
- `MTIMER` 放在 `CLINT base + RISCV_ACLINT_SWI_SIZE`

这和 `virt` 机型的经典布局是一致思路。

---

### 6.1 软件中断（MSWI）链路

1. Guest 写 MSIP 寄存器
2. `riscv_aclint_swi_write()` 检测 bit0
3. bit0=1 -> `qemu_irq_raise(...)`
4. bit0=0 -> `qemu_irq_lower(...)`
5. ACLINT 输出线连接到 CPU 的 `IRQ_M_SOFT`

最终 CPU 的 `MIP_MSIP` 被更新。

---

### 6.2 定时器中断（MTIMER）链路

1. Guest 写 `mtimecmp`
2. `riscv_aclint_mtimer_write_timecmp()` 计算触发时间
3. 到时后 `riscv_aclint_mtimer_cb()` 执行
4. raise 到 `IRQ_M_TIMER`

最终 CPU 的 `MIP_MTIP` 被更新。

此外你传了 `provide_rdtime=true`，意味着 ACLINT 会给 CPU 提供 `rdtime` 回调来源。

---

## 7. CPU 端是如何接收这些 IRQ 的？

核心在 `riscv_cpu_set_irq()`：

- 本地 IRQ（soft/timer/ext）进来后，更新 `mip` 位。
- `IRQ_S_EXT` 有单独路径，会合并 `external_seip` 与 `software_seip`。

你可以把这一步理解为：

**设备线电平变化 -> CPU 中断挂起寄存器（mip）变化 -> trap 逻辑决定是否进入中断处理。**

---

## 8. 三条典型端到端时序（非常关键）

### 8.1 外设中断（例如 UART）

理想完整链路应是：

1. UART 设备产生 IRQ
2. UART IRQ 输出连接到 PLIC 某个 source（比如 source 10）
3. PLIC 置 pending 并仲裁
4. PLIC 拉高目标 hart 的 `S_EXT/M_EXT`
5. CPU `mip.SEIP/MEIP` 置位
6. Guest 执行 claim -> ISR -> complete

> 你当前代码已完成 3~5 的机制；1~2 的“具体外设接线”还需补充。

---

### 8.2 软件 IPI（核间中断）

1. Hart0 写 Hart1 的 MSIP
2. ACLINT SWI raise Hart1 `IRQ_M_SOFT`
3. Hart1 `mip.MSIP` 置位并响应中断

这条链路不经过 PLIC。

---

### 8.3 定时器中断

1. Hart 写本 hart `mtimecmp`
2. QEMU 定时器到点触发回调
3. raise 到该 hart `IRQ_M_TIMER`
4. `mip.MTIP` 置位

也不经过 PLIC。

---

## 9. 补充

### 9.1 ACLINT
#### 9.1.1 SWI 和 MTIMER
在 RISC-V 的 ACLINT（Advanced Core Local Interruptor）规范中，`swi` 和 `mtimer` 是两个核心的功能模块。它们分别负责处理**核心间通信**和**系统时间管理**。

在你的代码中，它们被拆分成了两个独立的函数调用（`riscv_aclint_swi_create` 和 `riscv_aclint_mtimer_create`），这是 ACLINT 相比旧版 CLINT 的一大改进（允许内存布局更灵活）。

以下是详细解释：

##### 1. SWI (Software Interrupts / 软件中断)

* **全称**：Software Interrupt device。
* **代码对应**：`riscv_aclint_swi_create`。
* **核心作用**：实现 **IPI (Inter-Processor Interrupt，核间中断)**。
* **它是如何工作的？**
* SWI 模块包含一组寄存器（主要是 `SETSSIP` 或 `MSIP`），每个 CPU 核心（Hart）都有对应的一个。
* 当 CPU A 想要“叫醒”或通知 CPU B 做某事时，CPU A 会向 CPU B 对应的 SWI 寄存器写入 `1`。
* 这会立刻触发 CPU B 的软件中断引脚（通常对应 `mip` 寄存器的 MSIP 位）。


* **操作系统用它做什么？**
* **多核调度**：Linux 内核使用它在不同的 CPU 之间分发任务。
* **TLB 刷新**：当页表更新时，通知其他 CPU 刷新缓存。
* **Kick**：唤醒处于 `WFI` (Wait For Interrupt) 休眠状态的 CPU。



> **注意**：在你的代码中，`riscv_aclint_swi_create` 的最后一个参数是 `false`，这意味着创建的是 **M-mode (Machine level)** 的软件中断控制器（MSWI）。如果为 `true`，则是 S-mode (Supervisor level) 的（SSWI）。
---

###### MSWI 和 SSWI 区别

它们的根本区别在于**特权模式（Privilege Mode）和使用场景**。

简单来说：

* **MSWI (Machine-level SWI)**：给 **固件 (Firmware/OpenSBI)** 用的，最高权限。
* **SSWI (Supervisor-level SWI)**：给 **操作系统 (OS/Linux)** 用的，次高权限。

以下是详细对比：

###### 1. 核心区别表

| 特性 | **MSWI** (Machine SWI) | **SSWI** (Supervisor SWI) |
| --- | --- | --- |
| **全称** | Machine-level Software Interrupt | Supervisor-level Software Interrupt |
| **目标特权级** | **M-mode** (机器模式) | **S-mode** (监管者模式) |
| **控制的寄存器位** | `mip.MSIP` (Machine Soft Interrupt Pending) | `mip.SSIP` (Supervisor Soft Interrupt Pending) |
| **主要使用者** | OpenSBI, BIOS, RTOS (裸机) | Linux Kernel, FreeBSD |
| **触发方式** | 写 MSWI 设备寄存器 | 写 SSWI 设备寄存器 |
| **性能** | 较慢（如果 OS 用它需要陷入 M 模式） | **极快**（OS 直接读写，无上下文切换） |

###### 2. 深入解析：为什么需要 SSWI？

在 RISC-V 的早期设计（只有 CLINT，没有 ACLINT）中，**只有 MSWI**。这带来了一个性能问题。

###### 场景：Linux 想要给另一个 CPU 发送中断 (IPI)

**旧的方式（只有 MSWI 时）：**

1. **Linux (S-mode)** 想要发送 IPI。
2. 因为它没有权限直接操作 MSWI，它必须调用 **SBI指令 (ecall)** 陷入到 M-mode。
3. **OpenSBI (M-mode)** 捕获这个调用。
4. OpenSBI 操作 MSWI 寄存器，触发中断。
5. OpenSBI 返回 S-mode。
6. **目标 CPU** 收到中断。

> **缺点**：每次发中断都要在 S-mode 和 M-mode 之间反复横跳（Trap/Return），开销很大。

**新的方式（引入 ACLINT SSWI 后）：**

1. **Linux (S-mode)** 想要发送 IPI。
2. 硬件上直接把 **SSWI 设备** 的内存地址映射给 Linux。
3. Linux 直接写 SSWI 寄存器。
4. **目标 CPU** 立刻收到 S-mode 的软件中断。

> **优点**：完全绕过了 M-mode，无需 SBI 调用，性能大幅提升。

###### 3. 在 QEMU 代码中的体现

回到你之前的代码：

```c
riscv_aclint_swi_create(..., false); // 最后一个参数是 is_sswi

```

* **参数为 `false**`：创建的是 **MSWI**。这是系统的基础，因为 M-mode (OpenSBI) 启动时必须要有它来协调各个 CPU核。
* **参数为 `true**`：创建的是 **SSWI**。

**通常的系统设计：**
现代的高性能 RISC-V 机器（如 SiFive U74, C910 等）通常会**同时拥有**这两种设备：

1. **MSWI**：留给 OpenSBI 使用，用于启动、休眠唤醒等底层操作。
2. **SSWI**：留给 Linux 使用，用于调度、多核任务分发。

---

##### 2. MTIMER (Machine Timer / 机器定时器)

* **全称**：Machine-level Timer device。
* **代码对应**：`riscv_aclint_mtimer_create`。
* **核心作用**：提供**系统时基（System Time）**和**定时中断**。
* **它是如何工作的？**
它包含两个关键部分：
1. **`mtime` (Machine Time)**：这是一个 64 位的全局计数器，以固定的频率（`RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ`，通常是 10MHz 或 1MHz）不断增加。它代表了系统启动以来流逝的时间。
2. **`mtimecmp` (Machine Time Compare)**：每个 CPU 核心都有一个独立的比较寄存器。


* **触发机制**：当 `mtime` 的值 **大于或等于** 某个 CPU 的 `mtimecmp` 的值时，就会给那个 CPU 触发一个定时器中断（对应 `mip` 寄存器的 MTIP 位）。


* **操作系统用它做什么？**
* **任务切换**：这是实现分时多任务（Preemptive Multitasking）的基础。OS 设置一个未来的时间点（比如 10ms 后），时间一到，中断触发，OS 暂停当前程序，切换到下一个程序。
* **系统时钟**：像 `gettimeofday` 这样的函数，底层就是读取 `mtime` 的值来计算当前时间的。
* **Sleep**：实现 `sleep()` 或 `delay()` 函数。



---

##### 3. 总结与对比

为了方便理解，可以用“闹钟”和“拍肩膀”来做比喻：

| 特性 | **SWI (软件中断)** | **MTIMER (定时器)** |
| --- | --- | --- |
| **比喻** | **拍肩膀** | **闹钟** |
| **触发源** | **软件触发** (一个 CPU 写寄存器触发另一个 CPU) | **硬件自动触发** (时间到了自动触发) |
| **主要用途** | **通信**：告诉别的 CPU "嘿，醒醒，有活干了" | **计时**：告诉当前 CPU "时间片用完了，该换人了" |
| **依赖关系** | 依赖于 CPU 之间的互联 | 依赖于固定的时钟频率 (Timebase Freq) |
| **中断类型** | 异步、突发性 | 同步、周期性 |

##### 4. 为什么代码里要分开写？

在早期的 RISC-V 规范（SiFive CLINT）中，SWI 和 MTIMER 是捆绑在一个内存块里的（混在一起）。

但在新的 **ACLINT** 规范中，设计者将它们模块化了：

* 你可以把 SWI 放在内存的一个地方。
* 把 MTIMER 放在内存的另一个地方。
* 甚至可以有多个 MTIMER 模块。

所以 QEMU 提供了两个独立的函数 `riscv_aclint_swi_create` 和 `riscv_aclint_mtimer_create`，让你能更灵活地定义硬件布局。你的代码通过计算偏移量（`... + RISCV_ACLINT_SWI_SIZE`），将 MTIMER 紧挨着放在了 SWI 后面，这保持了与旧版 CLINT 类似的连续内存布局。
