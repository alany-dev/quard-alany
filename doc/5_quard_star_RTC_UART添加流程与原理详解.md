# quard_star 的 UART/RTC 添加全流程与实现原理

---

## 1. 先回答你的核心疑问

你问的是：

> “为什么传入的 plic 不是只有 8 个吗？为什么 UART/RTC 可以接到 10、11、12、13？”

这其实是把三种“数量”混在了一起。它们是三条独立维度：

1. **PLIC 实例数量（按 socket）**
   - 代码：`DeviceState *plic[QUARD_STAR_SOCKETS_MAX]`，`QUARD_STAR_SOCKETS_MAX = 8`
   - 含义：最多保存 8 个 **PLIC 设备实例指针**（每个 socket 一个），不是“8 路中断”。

2. **PLIC 输出 context 数量（按 hart 和模式）**
   - 来自 `hart-config`，例如一个 hart 常见是 `MS` 两个 context。
   - 8 个 hart 可能对应 16 个 context（M_EXT + S_EXT）。
   - 这决定了 PLIC 的 `enable/context` 寄存器规模，不是外设输入路数。

3. **PLIC 输入 source 数量（外设中断线）**
   - 代码里传的是 `QUARD_STAR_PLIC_NUM_SOURCES = 127`
   - 在 `sifive_plic` 中这就是 `num-sources`，并且注释明确写了“包含 source 0”。
   - 可用 source ID 是 `1..126`，所以 `10/11/12/13` 完全合法。

结论：

- 你现在接 `qdev_get_gpio_in(DEVICE(s->plic[0]), 10/11/12/13)` 没问题，因为这是在第 0 个 PLIC 实例上取第 10~13 号输入脚；这个实例一共有 127 路输入脚（含 0 号）。

---

## 2. 这个结论的源码证据

## 2.1 `8` 是 PLIC 实例槽位，不是 source 数

文件：`qemu-8.0.2/include/hw/riscv/quard_star.h`

- `DeviceState *plic[QUARD_STAR_SOCKETS_MAX];`
- `#define QUARD_STAR_SOCKETS_MAX 8`

这只是 machine state 里可挂多少个 PLIC 设备指针。

## 2.2 真正的 source 数是 `num-sources`

文件：`qemu-8.0.2/hw/riscv/quard_star.c`

- `sifive_plic_create(..., QUARD_STAR_PLIC_NUM_SOURCES, ...)`
- `QUARD_STAR_PLIC_NUM_SOURCES = 127`

文件：`qemu-8.0.2/hw/intc/sifive_plic.c`

- 属性定义注释：`num-sources` “including interrupt source 0”
- `qdev_init_gpio_in(dev, sifive_plic_irq_request, s->num_sources);`

这行是关键：PLIC 的输入 GPIO 数量直接按 `num_sources` 分配。

## 2.3 UART/RTC 接的是“输入 source 编号”

文件：`qemu-8.0.2/hw/riscv/quard_star.c`

- UART0: `qdev_get_gpio_in(DEVICE(s->plic[0]), 10)`
- UART1: `qdev_get_gpio_in(DEVICE(s->plic[0]), 11)`
- UART2: `qdev_get_gpio_in(DEVICE(s->plic[0]), 12)`
- RTC:   `qdev_get_gpio_in(DEVICE(s->plic[0]), 13)`

只要编号 `< num_sources` 就能接上。这里 `13 < 127`，当然合法。

---

## 3. UART/RTC 的添加全流程（按执行顺序）

下面只讲你改动涉及的主链路。

## 3.1 编译期开关

文件：`qemu-8.0.2/hw/riscv/Kconfig`

- `config QUARD_STAR` 新增：`select GOLDFISH_RTC`

作用：

- 确保 `goldfish_rtc` 设备类型会被编进来，后面 `sysbus_create_simple("goldfish_rtc", ...)` 才能实例化。

## 3.2 板级常量和地址图

文件：`qemu-8.0.2/include/hw/riscv/quard_star.h`

- 新增枚举项：`QUARD_STAR_UART1`、`QUARD_STAR_UART2`、`QUARD_STAR_RTC`
- 新增 IRQ 常量：`UART1=11`、`UART2=12`、`RTC=13`

文件：`qemu-8.0.2/hw/riscv/quard_star.c`

`quard_star_memmap[]` 新增：

- `UART1 @ 0x10001000`
- `UART2 @ 0x10002000`
- `RTC   @ 0x10003000`

## 3.3 先创建 PLIC（外设要接它）

函数：`quard_star_plic_create()`

关键调用：

```c
s->plic[i] = sifive_plic_create(...,
                                QUARD_STAR_PLIC_NUM_SOURCES,
                                ...);
```

这一刻完成两件事情：

1. 创建 PLIC MMIO 区。
2. 在 PLIC 内部分配 `num_sources` 路输入脚，等外设来接。

## 3.4 创建 UART0/1/2 并接入 PLIC

函数：`quard_star_serial_create()`

每路 UART 都是同样结构：

```c
serial_mm_init(system_memory,
               uart_base,
               0,
               qdev_get_gpio_in(DEVICE(s->plic[0]), uart_irq),
               399193,
               serial_hd(n),
               DEVICE_LITTLE_ENDIAN);
```

`serial_mm_init` 内部流程：

1. `qdev_new(TYPE_SERIAL_MM)` 创建 16550 MMIO 串口设备。
2. `sysbus_realize_and_unref` 完成设备初始化。
3. `sysbus_connect_irq(..., irq)` 把串口 IRQ 输出线接到你传入的 PLIC 输入脚。
4. `memory_region_add_subregion(address_space, base, mr)` 把串口寄存器窗口挂到 MMIO 地址上。

到这一步，UART 的“地址线”和“中断线”都建好了。

## 3.4.1 为什么 UART 和 内存有关？
这是一个非常好的问题，触及了嵌入式系统和 QEMU 仿真中最核心的概念之一：**MMIO（Memory Mapped I/O，内存映射输入输出）**。

简单来说，UART 使用 `get_system_memory` 是因为在 RISC-V（以及 ARM、x86 等大多数架构）中，**CPU 访问外设（如串口）的方式和访问内存（RAM）的方式是一样的，都是通过读写物理地址来实现的。**

我们需要把 UART 设备的寄存器“挂载”到系统全局的内存空间中，这样 CPU 才能找得到它。

以下是详细的原理解析：

### 1. 什么是 `get_system_memory()`？

在 QEMU 中，`get_system_memory()` 返回的是**全局系统内存区域（System Memory Region）的根节点。你可以把它想象成整个主板的物理地址空间地图**。

* 这张地图是从地址 `0x0` 到 `0xFFFFFFFFFFFFFFFF` 的巨大画布。
* 我们需要在这张画布上贴上不同的“贴纸”：
* 在 `0x80000000` 处贴上 DRAM（内存条）。
* 在 `0x00000000` 处贴上 ROM。
* 在 `0x10000000` 处贴上 **UART0**。



### 2. `serial_mm_init` 做了什么？

你的代码中调用了：

```c
serial_mm_init(system_memory, 
               quard_star_memmap[QUARD_STAR_UART0].base, 
               ...);

```

这个函数是 QEMU 提供的一个快捷方式（Helper Function），它的内部逻辑大致如下（伪代码流程）：

1. **创建设备**：实例化一个 UART 设备对象。
2. **创建内存子区域**：UART 设备内部有一小块 I/O 内存区域（用来代表它的接收/发送缓冲寄存器、状态寄存器等）。
3. **映射（Mapping）**：它调用 `memory_region_add_subregion`，将 UART 的这块小内存区域，**添加**到你传入的 `system_memory`（全局大地图）中，位置就是你指定的 `base` 地址。

**如果没有传入 `system_memory`，QEMU 就不知道要把这个 UART 设备放在总线地址空间的哪个“父节点”下，CPU 发出的读写请求也就无法路由到这个设备上。**

### 3. CPU 是如何与 UART 通信的？

为了让你更直观地理解，请看这个数据流过程：

1. **软件层**：你的操作系统或裸机代码想要打印字符 'A'。
2. **指令层**：CPU 执行一条写指令，例如 `SW` (Store Word)，目标地址是 UART0 的基地址 `0x10000000`，数据是 'A'。
3. **QEMU 模拟层**：
* QEMU 捕获到 CPU 往地址 `0x10000000` 写数据的动作。
* QEMU 查看 `system_memory`（就是刚才通过 `get_system_memory` 获取的那个对象）。
* 它发现 `0x10000000` 这个地址属于之前注册的 UART 设备区域（而不是 RAM）。
* QEMU **拦截**这次写入，不再往 RAM 里写，而是触发 UART 设备的**回调函数**。


4. **设备层**：UART 模拟逻辑接收到数据 'A'，将其输出到控制台。

### 总结

UART 用到 `get_system_memory` 的原因可以总结为三点：

1. **统一编址**：RISC-V 采用 MMIO，外设寄存器被视为内存地址的一部分。
2. **地址路由**：QEMU 需要一个根容器（System Memory）来管理所有的地址映射关系。
3. **挂载设备**：必须显式地告诉 QEMU，“把 UART 这个设备安插在系统内存总线的 `0x10000000` 这个位置上”，而 `system_memory` 就是代表这条总线的对象。

对比一下，如果不做这一步，UART 设备虽然被创建了，但它就像一个没有门牌号的房子，邮递员（CPU）永远无法把信（数据）送到它手里。

### 3.4.2 除了 RISC-V，其他也是这样的吗?

在计算机体系结构中，CPU 访问外设主要有两种方式：**统一编址（MMIO）** 和 **独立编址（PMIO）**。

### 1. 统一编址 (MMIO) —— RISC-V, ARM 的做法

绝大多数精简指令集（RISC）架构，包括 **RISC-V, ARM, MIPS, PowerPC**，都使用 **Memory Mapped I/O (MMIO)**。

* **特点**：内存和外设共享同一个地址空间（0x0000... 到 0xFFFF...）。
* **访问方式**：CPU 不需要专门的 I/O 指令。
* 想读内存？用 `Load` 指令。
* 想读串口状态？还是用 `Load` 指令。
* CPU 不关心目标地址是内存条还是 UART，它只管发地址，总线会根据地址映射决定谁来响应。


* **QEMU 中的表现**：正如你代码里写的，必须使用 `get_system_memory()`，把设备挂载到全局地址空间的大地图上。

### 2. 独立编址 (PMIO) —— x86 的特殊做法

**x86 (Intel/AMD)** 架构比较特殊，它保留了古老的 **Port Mapped I/O (PMIO)** 机制。

* **特点**：x86 有**两套**独立的地址空间：
   1. **内存地址空间**：也就是我们平时说的内存（4GB, 8GB...），用 `MOV` 指令访问。
   2. **I/O 端口地址空间**：一个独立的、只有 64KB 大小的空间（0x0000 - 0xFFFF），专门留给外设。


* **访问方式**：CPU 有专门的指令 `IN` 和 `OUT`。
* `MOV EAX, [0x1000]`：这是去读内存地址 0x1000。
* `IN AL, 0x3F8`：这是去读 I/O 端口 0x3F8（通常是 x86 上的串口 COM1）。


* **QEMU 中的表现**：
在 QEMU 编写 x86 设备驱动时，你会看到另一个函数：`get_system_io()`。
* `get_system_memory()`：对应内存空间。
* `get_system_io()`：对应那特殊的 64KB I/O 端口空间。


> **注意**：虽然 x86 支持端口映射，但现代 x86 电脑上的高速设备（如显卡、网卡、PCIe 设备）为了追求速度和容量，**也全部改用 MMIO 了**。只有老旧的设备（如传统串口、键盘控制器、RTC）为了兼容性还保留在 I/O 端口空间里。

### 总结对比表

| 特性 | RISC-V / ARM (你正在用的) | x86 (传统部分) |
| --- | --- | --- |
| **编址方式** | **MMIO (统一编址)** | **PMIO (独立编址)** + MMIO |
| **地址空间** | 只有 1 个：内存和外设混在一起 | 有 2 个：一个存内存，一个存 IO 端口 |
| **CPU 指令** | 统一用 `LB`, `SB`, `LW`, `SW` | 访问内存用 `MOV`，访问外设用 `IN`, `OUT` |
| **QEMU 挂载点** | `get_system_memory()` | 端口用 `get_system_io()`，显卡等用 `get_system_memory()` |
| **串口实现** | 映射到如 `0x10000000` 这样的高地址 | 通常固定在端口 `0x3F8` |

## 3.5 创建 RTC 并接入 PLIC

函数：`quard_star_rtc_create()`

```c
sysbus_create_simple("goldfish_rtc",
                     rtc_base,
                     qdev_get_gpio_in(DEVICE(s->plic[0]), QUARD_STAR_RTC_IRQ));
```

`sysbus_create_simple` 做的是“创建 + realize + map + connect_irq”的简写封装。  
所以 RTC 也同样完成：

1. MMIO 映射到 `0x10003000`
2. IRQ 连接到 PLIC source 13

---

## 4. 运行时原理（真正“怎么触发中断”）

## 4.1 UART 侧如何决定拉高中断

文件：`qemu-8.0.2/hw/char/serial.c`

核心函数：`serial_update_irq(SerialState *s)`

逻辑：

1. 看 IER 是否使能对应中断（RDI/THRI/RLSI/MSI）。
2. 看 LSR/FIFO/状态位是否满足触发条件。
3. 若有可投递中断，`qemu_irq_raise(s->irq)`；否则 `qemu_irq_lower(s->irq)`。

这里的 `s->irq`，就是你在 `serial_mm_init` 里传入的 `qdev_get_gpio_in(DEVICE(s->plic[0]), 10/11/12)`。

所以 UART 实际是在直接拉 PLIC 的输入脚。

## 4.2 RTC 侧如何决定拉高中断

文件：`qemu-8.0.2/hw/rtc/goldfish_rtc.c`

关键链路：

1. Guest 写 `ALARM_LOW/HIGH` 设置闹钟。
2. 到时触发 `goldfish_rtc_interrupt()`，置 `irq_pending=1`。
3. `goldfish_rtc_update()` 调 `qemu_set_irq(s->irq, (irq_pending & irq_enabled) ? 1 : 0)`。

这个 `s->irq` 就是你接到 PLIC source 13 的那根线。

## 4.3 PLIC 收到输入后如何上报 CPU

文件：`qemu-8.0.2/hw/intc/sifive_plic.c`

主流程：

1. 输入脚电平变化进入 `sifive_plic_irq_request(irq, level)`。
2. 置位/清位 `pending[irq]`。
3. `sifive_plic_update()` 重新仲裁：
   - 只在 `pending & enable & ~claimed` 里选候选
   - 选择优先级高于 threshold 的最大者
4. 根据 context 模式，拉高对应 hart 的 `IRQ_M_EXT` 或 `IRQ_S_EXT`。
5. Guest 读 claim 寄存器取走 IRQ（pending->claimed），写 complete 清 claimed。

这就是标准 PLIC 的 claim/complete 闭环。

---

## 5. 再把“8、127、10~13”用一个图钉死

假设你当前是 `-smp 8` 且单 socket，模型可理解为：

1. PLIC 实例：`s->plic[0]`（只用到一个实例）
2. 每个实例输入 source：`0..126`（共 127）
3. 你占用了 source：`10/11/12/13`

抽象连线：

```text
UART0 irq --> PLIC[0] input#10
UART1 irq --> PLIC[0] input#11
UART2 irq --> PLIC[0] input#12
RTC   irq --> PLIC[0] input#13
                 |
                 v
           PLIC仲裁/claim
                 |
                 v
           Hart 的 M_EXT/S_EXT
```

所以“只有 8 个 plic”这个说法，准确说应该是“最多 8 个 PLIC 实例槽位”。  
与你现在用到的 source 10~13 没冲突，因为 source 编号属于“单实例内部编号空间”。

---

## 6. 这次实现里最关键的正确点

1. **先建 PLIC，再建 UART/RTC**
   - 否则 `qdev_get_gpio_in(DEVICE(s->plic[0]), X)` 无对象可接。
2. **source 号选在合法范围**
   - 当前 `10~13` 在 `1..126` 范围内。
3. **UART/RTC 都走同一外部中断主干**
   - 便于后续在 OS 侧统一通过 PLIC 驱动处理。

---