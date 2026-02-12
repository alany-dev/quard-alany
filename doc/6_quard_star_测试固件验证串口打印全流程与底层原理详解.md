# quard-star 测试固件验证串口打印全流程与底层原理详解（基于当前改动）

## 1. 目的与验收标准

本文面向当前仓库中的最新改动，目标是建立一条可重复、可解释、可定位问题的“固件串口打印验证”流程。  
验证对象不是操作系统，而是最小低级固件（`boot/start.s`）通过 MMIO 向 UART0 发送字符，并在宿主机终端看到输出。

本轮验证通过标准：

1. 固件成功编译并打包到 `output/lowlevelboot/fw.bin`。
2. QEMU 以 `-bios none` + `if=pflash` 方式启动 `quard-star`。
3. 终端出现 `Hello Quard Star board!`。
4. 指令级链路可追溯：复位入口 -> Flash 固件 -> UART MMIO 写 -> QEMU 串口后端输出。

---

## 2. 当前改动基线（本说明依赖）

当前仓库中和本测试直接相关的改动点：

1. `build.sh`  
   新增低级固件构建流程：`start.s -> start.o -> lowlevel_fw.elf -> lowlevel_fw.bin -> fw.bin`。
2. `run.sh`  
   启动参数切换为：
   - `-bios none`
   - `-drive if=pflash,bus=0,unit=0,format=raw,file=output/lowlevelboot/fw.bin`
   - `-nographic`
3. `boot/start.s`  
   新增最小裸机汇编，hart0 直接向 `0x10000000` 连续 `sb` 输出字符串。
4. `boot/boot.lds`  
   固件链接地址设置为 Flash 基地址 `0x20000000`。
5. `qemu-8.0.2/hw/riscv/quard_star.c` 与 `qemu-8.0.2/include/hw/riscv/quard_star.h`  
   已存在/已改造的板级定义中包含：
   - Flash 映射：`0x20000000`，大小 `0x02000000`（32 MiB）
   - UART0 映射：`0x10000000`
   - UART 由 `serial_mm_init()` 创建，IRQ 接到 PLIC。

---

## 3. 从源码到镜像：固件构建与封装链路

### 3.1 工具链路径注入

`build.sh` 先执行：

```bash
export PATH=$PATH:$SHELL_FOLDER/riscv64-elf-ubuntu-24.04-gcc/bin
```

确保 `riscv64-unknown-elf-gcc/objcopy/objdump` 可直接调用。

### 3.2 汇编与链接

关键命令（来自 `build.sh`）：

```bash
riscv64-unknown-elf-gcc -x assembler-with-cpp -c start.s -o output/lowlevelboot/start.o
riscv64-unknown-elf-gcc -nostartfiles -T./boot.lds \
  -Wl,-Map=output/lowlevelboot/lowlevel_fw.map \
  -Wl,--gc-sections output/lowlevelboot/start.o \
  -o output/lowlevelboot/lowlevel_fw.elf
```

要点：

1. `-nostartfiles`：不链接 C 运行时启动代码，入口完全由 `_start` 控制。
2. `boot.lds` 中 `flash ORIGIN = 0x20000000`，因此 `_start` 放在 Flash 基地址。
3. 从 `lowlevel_fw.map` 可见：`.text` 在 `0x20000000`，`_start` 也在该地址。

### 3.3 ELF 转 BIN

```bash
riscv64-unknown-elf-objcopy -O binary -S lowlevel_fw.elf lowlevel_fw.bin
```

`lowlevel_fw.bin` 是纯二进制代码镜像，不含 ELF 元数据。

### 3.4 生成 pflash 后端文件 `fw.bin`

```bash
dd of=fw.bin bs=1k count=32k if=/dev/zero
dd of=fw.bin bs=1k conv=notrunc seek=0 if=lowlevel_fw.bin
```

底层含义：

1. `count=32k`、`bs=1k` 生成 32 MiB 文件，正好匹配板级 Flash 大小 `0x02000000`。
2. 第二条命令把固件写到偏移 0（即 Flash 基址 `0x20000000` 对应的第一个字节）。
3. 因为 reset stub 跳转目标是 `0x20000000`，所以固件必须从偏移 0 开始。

---

## 4. QEMU 启动到固件执行：控制流全链路

### 4.1 启动命令的作用拆解

`run.sh` 核心命令：

```bash
qemu-system-riscv64 \
  -M quard-star \
  -m 1G \
  -smp 8 \
  -bios none \
  -drive if=pflash,bus=0,unit=0,format=raw,file=output/lowlevelboot/fw.bin \
  -nographic \
  --parallel none
```

重点参数：

1. `-bios none`：不加载 OpenSBI 默认固件，控制权留给板级 reset 逻辑与 pflash 固件。
2. `if=pflash,bus=0,unit=0`：把 `fw.bin` 作为 0 号并行 Flash 设备后端。
3. `-nographic`：关闭图形，串口/控制台走终端，便于直接观察打印。

### 4.2 板级初始化顺序（`quard_star_machine_init`）

初始化顺序是：

1. 创建 CPU（hart array）。
2. 创建内存（DRAM/SRAM/MROM）。
3. 创建 Flash 设备并映射。
4. 创建 PLIC。
5. 创建 ACLINT。
6. 创建 UART0/1/2。
7. 创建 RTC。

顺序上先有 Flash 与 UART 映射，再运行固件，这样 CPU 执行时访问地址可被正确路由。

### 4.3 MROM reset stub 如何把 PC 引导到 Flash

`quard_star_memory_create()` 调用：

```c
riscv_setup_rom_reset_vec(...,
    quard_star_memmap[QUARD_STAR_FLASH].base,  // 0x20000000
    quard_star_memmap[QUARD_STAR_MROM].base,   // 0x0
    quard_star_memmap[QUARD_STAR_MROM].size,   // 0x8000
    ...)
```

`riscv_setup_rom_reset_vec()` 会在 MROM 填入一段小 reset 程序，其核心行为是：

1. 读取 `mhartid` 到 `a0`。
2. 从内嵌数据表取出 `start_addr`（这里是 `0x20000000`）。
3. `jr t0` 跳转到 Flash 基址执行。

所以主启动路径是：  
**MROM reset stub -> Flash `0x20000000` -> `_start`**。

### 4.4 一个实现细节：默认 resetvec 为 `0x1000`

RISC-V CPU 默认 `resetvec` 是 `0x1000`（`target/riscv/cpu_bits.h`）。  
本板级没有显式改写 `resetvec`，但 MROM reset stub 放在 `0x0`。

在实测指令日志中，首条取指发生在 `0x1000`，出现非法指令后陷入 `0x0`，随后执行 MROM reset stub 并成功跳转 Flash。  
这能工作，但属于“依赖当前复位/异常初值行为”的路径，工程上建议后续显式设置 hart resetvec 与 MROM stub 地址一致，减少歧义。

---

## 5. UART 打印底层原理：从 `sb` 到宿主终端

### 5.1 固件执行逻辑（`boot/start.s`）

固件核心逻辑：

1. `csrr a0, mhartid` 读取 hart ID。
2. 仅 hart0 继续执行，其他 hart 进入死循环。
3. 计算 UART0 基地址 `0x10000000`。
4. 连续执行 `sb t1, 0(t0)` 写 ASCII 到 UART 数据寄存器（offset 0）。
5. 输出完毕后进入死循环，便于观察状态。

### 5.2 为什么写 `0x10000000` 就能打印

因为 `quard_star_memmap` 中 UART0 映射为：

- Base: `0x10000000`
- Size: `0x100`

`quard_star_serial_create()` 中调用：

```c
serial_mm_init(system_memory, 0x10000000, 0, irq, 399193, serial_hd(0), DEVICE_LITTLE_ENDIAN);
```

这会把 `serial-mm` 设备的 MMIO 区域挂到系统物理地址空间 `0x10000000`。

CPU 写该地址时，QEMU 不会写 RAM，而会命中 UART 设备回调。

### 5.3 serial-mm 写路径

关键调用链：

1. 访存命中 `serial_mm_write()`
2. `addr >> regshift`（这里 `regshift = 0`，所以 offset 不变）
3. 转入 `serial_ioport_write(..., addr=0, val=byte)`
4. `addr=0` 且 `DLAB=0` 时，写入 THR（发送保持寄存器）
5. `serial_xmit()` 调用 `qemu_chr_fe_write()` 输出到后端字符设备

因此固件的 `sb` 指令最终转化为 host 终端的一次字符输出。

### 5.4 中断在这个测试里是否参与

本测试的“打印成功”不依赖 UART 中断：

1. 固件没有配置 IER，也没处理中断。
2. QEMU 串口模型在写 THR 后可直接进入发送路径。
3. PLIC 连接主要用于完整系统场景（驱动中断化收发），不是本最小打印用例必需条件。

---

