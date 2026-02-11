# quard_star 自定义开发板设计全过程与关键 QEMU API 详解

> 说明：本文档基于**当前暂存区（`git diff --cached`）**内容整理，描述的是你目前这版 `quard-star` 机器模型的设计状态，而不是未来规划版。

## 1. 现阶段改动总览（与 `quard-star` 直接相关）

你当前已在三层把 `quard-star` 打通：

1. **构建与运行入口层**
   - 新增 `build.sh`：统一 QEMU 配置、编译、安装到 `output/qemu/`。
   - 新增 `run.sh`：用 `-M quard-star -nographic -m 1G -smp 8` 直接启动。
   - `README.md` 更新了快速开始流程。

2. **QEMU 构建系统接入层**
   - `hw/riscv/Kconfig` 增加 `CONFIG_QUARD_STAR`。
   - `hw/riscv/meson.build` 增加 `quard_star.c` 编译入口。
   - `configs/devices/riscv32-softmmu/default.mak` 与 `riscv64-softmmu/default.mak` 打开 `CONFIG_QUARD_STAR=y`。

3. **机器模型实现层**
   - 新增 `include/hw/riscv/quard_star.h`。
   - 新增 `hw/riscv/quard_star.c`。

这三层齐备后，`qemu-system-riscv64` 已可以识别 `-M quard-star` 并进入你定义的 machine 初始化逻辑。

---

## 2. 设计全过程（按实现顺序拆解）

## 阶段 A：先把板子“编进 QEMU”

### A.1 `Kconfig` 声明机型能力

你新增了：

```kconfig
config QUARD_STAR
    bool
    select SERIAL
```

这一步告诉 QEMU：存在一个新板型 `QUARD_STAR`，并且依赖串口框架。

### A.2 Meson 加入源文件

```meson
riscv_ss.add(when: 'CONFIG_QUARD_STAR', if_true: files('quard_star.c'))
```

这是把实现代码真正纳入 `hw/riscv` 构建目标。

### A.3 默认设备配置启用机型

在 riscv32/riscv64 的 `default.mak` 中都加入：

```make
CONFIG_QUARD_STAR=y
```

到这里，编译系统路径闭环完成。

---

## 阶段 B：定义 machine 类型与状态对象

你在 `quard_star.h` 中完成了 machine 基础建模：

- `QUARD_STAR_CPUS_MAX = 8`
- `QUARD_STAR_SOCKETS_MAX = 8`
- `TYPE_RISCV_QUARD_STAR_MACHINE = MACHINE_TYPE_NAME("quard-star")`
- `QuardStarState` 继承 `MachineState`
- `QuardStarState` 内含 `RISCVHartArrayState soc[QUARD_STAR_SOCKETS_MAX]`

这说明你的设计采用了 QEMU RISC-V 里常见的“**每个 socket 一个 Hart Array**”建模方式，而不是手工逐个 CPU 直接 new。

同时你定义了初始内存区枚举：

- `MROM @ 0x00000000`
- `SRAM @ 0x00008000`
- `UART0 @ 0x10000000`
- `DRAM @ 0x80000000`

这一步把板级地址空间框架搭了出来。

---

## 阶段 C：CPU/Socket 拓扑创建

`quard_star_cpu_create()` 的逻辑是当前实现的核心之一：

1. 通过 `riscv_socket_count(machine)` 获取 socket 数（非 NUMA 时为 1）。
2. 限制不超过 `QUARD_STAR_SOCKETS_MAX`。
3. 对每个 socket：
   - `riscv_socket_check_hartids()` 校验 hartid 连续性。
   - `riscv_socket_first_hartid()` 求该 socket 起始 hartid。
   - `riscv_socket_hart_count()` 求 hart 数量。
4. 通过 QOM 创建 `TYPE_RISCV_HART_ARRAY` 子对象。
5. 设置属性：
   - `cpu-type`
   - `hartid-base`
   - `num-harts`
6. `sysbus_realize()` 完成实例化。

这套流程意味着：你的 board 已具备基础 SMP/NUMA 兼容接口，而不是固定死核模型。

---

## 阶段 D：内存区域创建与映射

`quard_star_memory_create()` 完成了三段主存储对象初始化：

- `memory_region_init_ram()` 创建 DRAM
- `memory_region_init_ram()` 创建 SRAM
- `memory_region_init_rom()` 创建 MROM

并统一挂到 `get_system_memory()` 返回的系统根地址空间：

- `memory_region_add_subregion(system_memory, base, region)`

这代表你的地址译码主干已经建立。

### 当前实现里的关键注意点

- `DRAM` size 目前为 `0x80`（128 字节），这是“占位级”容量；
- 运行参数里是 `-m 1G`，与板级映射尚未对齐；
- 这在后续应改为根据 `machine->ram_size` 建立 DRAM 映射。

---

## 阶段 E：复位向量与最小启动链路

你调用了：

```c
riscv_setup_rom_reset_vec(machine, &s->soc[0],
    mrom_base, mrom_base, mrom_size,
    0x0, 0x0);
```

它会在 MROM 写入 reset stub（含 `mhartid` 读取与跳转逻辑），形成“上电后可执行”的最小启动入口。

目前 `kernel_entry`、`fdt_load_addr` 都是 `0x0`，所以这是“先打通复位执行链”的阶段，还未进入完整固件/内核加载流程。

---

## 阶段 F：MachineClass 与类型注册

`quard_star_machine_class_init()` 里你设置了关键 machine 元数据：

- `mc->desc`
- `mc->init = quard_star_machine_init`
- `mc->max_cpus = 8`
- `mc->default_cpu_type = TYPE_RISCV_CPU_BASE`
- NUMA 相关回调：
  - `possible_cpu_arch_ids`
  - `cpu_index_to_instance_props`
  - `get_default_cpu_node_id`
- `mc->numa_mem_supported = true`

最后通过 `TypeInfo + type_register_static + type_init` 注册为 `-M quard-star` 可见机型。

这一步完成了 QOM 生命周期接入。

---

## 3. 关键 QEMU API 详解（结合你当前代码）

## 3.1 QOM 类型系统

### `TypeInfo`

- 作用：描述一个 QOM 类型的“类信息”（name/parent/class_init/instance_size 等）。
- 在你的代码中：`quard_star_machine_typeinfo` 把 `quard-star` 声明成 `TYPE_MACHINE` 子类。

### `type_register_static(const TypeInfo *info)`

- 作用：把类型注册进 QOM 类型表。
- 你在 `quard_star_machine_init_register_types()` 调用它完成注册。

### `type_init(fn)`

- 本质：`module_init(fn, MODULE_INIT_QOM)`。
- 作用：在 QEMU 初始化阶段自动执行 `fn`，避免手工调用注册函数。

---

## 3.2 对象创建与属性配置

### `object_initialize_child(parent, propname, child, type)`

- 作用：在父对象下初始化一个“具名子对象属性”。
- 你的用法：把每个 `soc[i]` 初始化成 `TYPE_RISCV_HART_ARRAY`。

### `object_property_set_str/int(...)`

- 作用：给 QOM 对象写属性。
- 你的用法：设置 HartArray 的 `cpu-type` / `hartid-base` / `num-harts`。

这些属性在 `riscv_hart.c` 中由 `DEFINE_PROP_*` 定义，属于标准的 QDEV 属性注入流程。

---

## 3.3 设备实例化

### `sysbus_realize(SysBusDevice *dev, Error **errp)`

- 作用：把 `SysBusDevice` 真正 realize（底层调用 `qdev_realize`）。
- 在你的场景：触发 `RISCVHartArray` 的 realize，进而创建实际 CPU 对象数组。

如果缺少 realize，设备只存在“对象壳”，不会进入可运行状态。

---

## 3.4 RISC-V NUMA/Socket 辅助 API

### `riscv_socket_count(ms)`

- 非 NUMA 场景返回 1；有 NUMA 时返回 node 数。

### `riscv_socket_first_hartid(ms, socket_id)` / `riscv_socket_hart_count(...)`

- 用于按 socket 推导 hart 区间。

### `riscv_socket_check_hartids(ms, socket_id)`

- 校验某 socket 的 hartid 是否连续。

你当前 CPU 创建流程依赖这组 API，因此天生兼容 `-smp` 与 NUMA 拓扑约束。

---

## 3.5 内存子系统 API

### `get_system_memory()`

- 返回系统根 MemoryRegion（全局物理地址空间容器）。

### `memory_region_init_ram()` / `memory_region_init_rom()`

- 分别创建 RAM/ROM 区域；ROM 等价于 RAM + 只读属性。

### `memory_region_add_subregion(container, offset, subregion)`

- 把子区域挂入容器地址空间。
- 不允许无优先级重叠映射（除非使用 overlap 版本）。

你的 `MROM/SRAM/DRAM` 都是按这个机制完成地址映射。

---

## 3.6 启动向量 API

### `riscv_setup_rom_reset_vec(...)`

- 作用：在 ROM 写入 reset vector + 固件信息结构。
- 关键参数：
  - `start_addr`：reset stub 最终跳转目标；
  - `rom_base/rom_size`：向量写入位置与空间；
  - `kernel_entry` / `fdt_load_addr`：后续引导链需要的入口地址信息。

你现在传入 `0x0/0x0`，表示“先具备 reset 执行能力，再逐步接入固件/内核/FDT”。

---

## 4. 当前设计成熟度评估

## 已完成

- `quard-star` 机型已可被 QEMU 编译并识别。
- 机器 class/type 注册链完整。
- CPU HartArray 创建与 NUMA 回调已接通。
- MROM/SRAM/DRAM 的基础 MemoryRegion 映射已建立。
- reset vector 生成路径已打通。

## 尚未完成（从当前代码可直接观察）

- UART 仅有地址/IRQ 预留，尚未实例化并连中断。
- ACLINT/APLIC 头文件已包含，但未完成中断控制器对象创建与 wiring。
- DRAM 映射容量尚未与 `-m` 参数统一。
- FDT 生成与加载链路尚未实装。
- 固件/内核装载流程（OpenSBI、kernel、initrd）未接入。

---

## 5. 建议的下一步实现顺序（可直接执行）

1. **先修正 DRAM 映射模型**
   - 把 DRAM 大小从固定常量改为 `machine->ram_size`。

2. **补中断基础设施**
   - 先做 ACLINT（timer/software IRQ），再做 APLIC/PLIC 外设中断汇聚。

3. **落地 UART0 实体**
   - 把串口设备实例化到 `0x10000000`，连到对应 IRQ。

4. **补 FDT 与启动参数**
   - 生成最小 DTS 节点（cpus/memory/chosen/uart/intc），让固件/内核可识别硬件。

5. **接入固件 + 内核加载路径**
   - 与 reset vector 参数联动，形成“可加载镜像”的完整启动链。

---

## 6. 现阶段建议的验证清单

```bash
# 1) 重新编译
sudo ./build.sh

# 2) 启动自定义板
./run.sh

# 3) 调试建议（可选）
./output/qemu/bin/qemu-system-riscv64 -M quard-star -nographic -m 1G -smp 8 -d guest_errors
```

重点关注：是否进入 reset 执行路径、是否出现地址映射冲突、是否有未实现设备访问告警。

