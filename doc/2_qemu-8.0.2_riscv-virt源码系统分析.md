# QEMU-8.0.2 `riscv-virt` 源码系统分析（深度版）

> 分析目标：系统梳理 `qemu-8.0.2` 中 `virt` 机器模型（`-M virt`）的构建接入、对象模型、地址空间、FDT/ACPI 生成、启动链路与关键 QEMU API 调用关系。  
> 分析范围：`hw/riscv/virt.c`、`include/hw/riscv/virt.h`、`hw/riscv/boot.c`、`hw/riscv/numa.c`、`hw/riscv/virt-acpi-build.c` 及相关中断/总线头文件。

---

## 1. 源码入口与构建接入

## 1.1 构建系统接入点

- `qemu-8.0.2/hw/riscv/Kconfig:27`：`config RISCV_VIRT` 定义 `virt` 机型能力，`select` 了 `RISCV_ACLINT`、`RISCV_APLIC`、`RISCV_IMSIC`、`SIFIVE_PLIC`、`FW_CFG_DMA`、`PLATFORM_BUS`、`ACPI` 等依赖。
- `qemu-8.0.2/hw/riscv/meson.build:6`：`CONFIG_RISCV_VIRT` 时编译 `virt.c`；`CONFIG_ACPI` 时编译 `virt-acpi-build.c`。
- `qemu-8.0.2/configs/devices/riscv64-softmmu/default.mak:14`、`qemu-8.0.2/configs/devices/riscv32-softmmu/default.mak:14`：默认启用 `CONFIG_RISCV_VIRT=y`。

## 1.2 QOM 类型注册入口

`virt.c` 尾部注册路径：

1. `virt_machine_typeinfo`（`.name = MACHINE_TYPE_NAME("virt")`）
2. `virt_machine_init_register_types()` 调 `type_register_static(...)`
3. `type_init(virt_machine_init_register_types)` 进入 QOM 初始化阶段

对应文件：`qemu-8.0.2/hw/riscv/virt.c:1718`。

---

## 2. 核心数据结构与常量模型

## 2.1 `RISCVVirtState`

定义位置：`qemu-8.0.2/include/hw/riscv/virt.h:43`。

关键字段：

- `soc[VIRT_SOCKETS_MAX]`：每个 socket 一个 `RISCVHartArrayState`。
- `irqchip[VIRT_SOCKETS_MAX]`：每 socket 一个外部中断控制器实例（PLIC 或 APLIC）。
- `flash[2]`：两片 CFI NOR（对应低/高半区）。
- `fw_cfg`：fw_cfg MMIO 设备。
- `have_aclint`、`aia_type`、`aia_guests`、`acpi`：机型行为开关。
- `memmap`：地址布局引用（供 ACPI/DT 等复用）。

## 2.2 规模上限与中断编号

定义位置：`qemu-8.0.2/include/hw/riscv/virt.h:27`。

- `VIRT_CPUS_MAX_BITS = 9` → `VIRT_CPUS_MAX = 512`
- `VIRT_SOCKETS_MAX_BITS = 2` → `VIRT_SOCKETS_MAX = 4`
- IRQ 约定：
  - `UART0_IRQ = 10`
  - `RTC_IRQ = 11`
  - `VIRTIO_IRQ = 1..8`
  - `PCIE_IRQ = 32..35`
  - `VIRT_PLATFORM_BUS_IRQ = 64..95`

## 2.3 中断架构模式

`RISCVVirtAIAType`（`virt.h:37`）：

- `none`：传统 PLIC
- `aplic`：AIA 仅 APLIC（wired interrupt）
- `aplic-imsic`：APLIC + IMSIC（wired + MSI）

---

## 3. 地址空间设计（`virt_memmap`）

定义位置：`qemu-8.0.2/hw/riscv/virt.c:77`。

核心映射（节选）：

- `VIRT_MROM`：`0x00001000`，`0xF000`
- `VIRT_TEST`：`0x00100000`，`0x1000`
- `VIRT_RTC`：`0x00101000`，`0x1000`
- `VIRT_CLINT`：`0x02000000`，`0x10000`
- `VIRT_ACLINT_SSWI`：`0x02F00000`，`0x4000`
- `VIRT_PLIC`：`0x0C000000`，`VIRT_PLIC_SIZE(VIRT_CPUS_MAX*2)`
- `VIRT_APLIC_M/S`：`0x0C000000 / 0x0D000000`
- `VIRT_UART0`：`0x10000000`
- `VIRT_VIRTIO`：`0x10001000` 起，每个 `0x1000`，共 8 个
- `VIRT_FW_CFG`：`0x10100000`，`0x18`
- `VIRT_FLASH`：`0x20000000`，`0x04000000`（64MB，总共两片）
- `VIRT_IMSIC_M/S`：`0x24000000 / 0x28000000`
- `VIRT_PCIE_ECAM`：`0x30000000`，`0x10000000`
- `VIRT_PCIE_MMIO`：`0x40000000`，`0x40000000`
- `VIRT_DRAM`：`0x80000000`，size=0（表示实际大小来自 `machine->ram_size`）

补充：

- RV32 高位 PCIe MMIO 固定：`0x300000000 + 4GiB`
- RV64 高位 PCIe MMIO：大小固定 16GiB，基址按 `dram_end` 向上 16GiB 对齐。

---

## 4. `virt_machine_init()` 全流程（设备实例化主链）

入口位置：`qemu-8.0.2/hw/riscv/virt.c:1332`。

可抽象为：

```text
virt_machine_init
├─ 校验 socket/hart 拓扑
├─ 为每个 socket 创建 RISCVHartArray
├─ 创建本地中断（CLINT/ACLINT）
├─ 创建外部中断控制器（PLIC 或 AIA）
├─ 计算高位 PCIe MMIO 窗口
├─ 映射 RAM + MROM
├─ 初始化 fw_cfg
├─ 创建设备：test/virtio/gpex/platform-bus/uart/rtc/flash
├─ 生成或加载 DTB
└─ 注册 machine_done 通知器（做固件/内核/FDT/reset vector 收尾）
```

## 4.1 CPU 与 socket 初始化

核心 API：

- `riscv_socket_count()` / `riscv_socket_first_hartid()` / `riscv_socket_hart_count()` / `riscv_socket_check_hartids()`（来自 `numa.c`）
- `object_initialize_child(... TYPE_RISCV_HART_ARRAY)`
- `object_property_set_str/int` 设置 `cpu-type` / `hartid-base` / `num-harts`
- `sysbus_realize()` 真正实例化 HartArray

这一段与 `quard_star` 的 CPU 创建思路一致，但 `virt` 后续接了完整中断与外设体系。

## 4.2 本地中断控制器（每 socket）

在 `!kvm_enabled()` 时创建：

- `aclint=on`：
  - 普通模式：MSWI + MTIMER + SSWI
  - `aia=aplic-imsic`：仅创建 MTIMER（MSWI/SSWI 被 AIA 路径替代）
- `aclint=off`：兼容 CLINT 语义（实装仍复用 ACLINT 组件 API）

实现调用：`riscv_aclint_swi_create()`、`riscv_aclint_mtimer_create()`。

## 4.3 外部中断控制器（每 socket）

- `aia=none`：`virt_create_plic()` → `sifive_plic_create()`
- `aia=aplic/aplic-imsic`：`virt_create_aia()`
  - `aplic-imsic` 时先创建 M/S 级 IMSIC，再创建 M/S 级 APLIC（MSI 模式）

对应：`qemu-8.0.2/hw/riscv/virt.c:1130`、`:1158`。

## 4.4 RAM / ROM / fw_cfg / 外设

- RAM：`memory_region_add_subregion(system_memory, VIRT_DRAM.base, machine->ram)`
- MROM：`memory_region_init_rom(... "riscv_virt_board.mrom", ...)`
- fw_cfg：`create_fw_cfg()`，并 `rom_set_fw(s->fw_cfg)`
- test：`sifive_test_create()`
- virtio-mmio：循环创建 8 个 `virtio-mmio`
- PCIe：`gpex_pcie_init(...)`
- platform-bus：`create_platform_bus(...)`
- UART：`serial_mm_init(...)`
- RTC：`sysbus_create_simple("goldfish_rtc", ... )`
- pflash：`virt_flash_create()` + `pflash_cfi01_legacy_drive()` + `virt_flash_map()`

## 4.5 DTB 来源

- 若 `-dtb` 指定：`load_device_tree(machine->dtb, ...)`
- 否则：`create_fdt(...)` 自动生成

最终注册：

- `s->machine_done.notify = virt_machine_done`
- `qemu_add_machine_init_done_notifier(...)`

---

## 5. FDT 生成链路（`create_fdt*`）逐层解析

入口：`qemu-8.0.2/hw/riscv/virt.c:1021`。

## 5.1 根节点与 `/soc`

- `/`：`model = "riscv-virtio,qemu"`，`compatible = "riscv-virtio"`
- 设置 root 与 `/soc` 的 `#address-cells/#size-cells`

## 5.2 `create_fdt_sockets()`：CPU/内存/中断主干

执行顺序：

1. 建 `/cpus`、`/cpus/cpu-map`
2. 为每个 socket 建立：
   - `create_fdt_socket_cpus()`：`/cpus/cpu@X`、`interrupt-controller` 子节点
   - `create_fdt_socket_memory()`：`/memory@...`
   - `create_fdt_socket_clint()` 或 `create_fdt_socket_aclint()`（非 KVM）
3. 若 `aia=aplic-imsic`：`create_fdt_imsic()`
4. 每 socket 追加 `create_fdt_socket_plic()` 或 `create_fdt_socket_aplic()`
5. 写入 NUMA 距离矩阵：`riscv_socket_fdt_write_distance_matrix()`

### 5.2.1 CPU 子节点细节

每个 CPU 节点会写入：

- `mmu-type`（由 `satp_mode` 推导）
- `riscv,isa`（`riscv_isa_string()`）
- `riscv,cbom-block-size` / `riscv,cboz-block-size`（若扩展开启）
- `reg`（hartid）
- `numa-node-id`（NUMA 场景）

并创建 `interrupt-controller` 子节点，类型为 `riscv,cpu-intc`。

### 5.2.2 PLIC / APLIC / IMSIC FDT 差异

- PLIC 路径：`compatible = sifive,plic-1.0.0 / riscv,plic0`
- APLIC 路径：`compatible = riscv,aplic`
- IMSIC 路径：`compatible = riscv,imsics`，并携带 `riscv,num-ids`、`group-index-bits`、`guest-index-bits` 等属性

`aia=aplic-imsic` 时，PCIe FDT 节点额外带 `msi-parent`。

## 5.3 外设与总线节点

- `create_fdt_virtio()`：8 个 `virtio,mmio`
- `create_fdt_pcie()`：ECAM/ranges/interrupt-map
- `create_fdt_reset()`：`/soc/test@...` + `/reboot` + `/poweroff`
- `create_fdt_uart()`：`ns16550a`，并写 `/chosen/stdout-path`
- `create_fdt_rtc()`：`google,goldfish-rtc`
- `create_fdt_flash()`：`cfi-flash`
- `create_fdt_fw_cfg()`：`qemu,fw-cfg-mmio`
- `create_fdt_pmu()`：`riscv,pmu`

最后在 `/chosen` 写入 `rng-seed`。

## 5.4 PCIe IRQ swizzle

`create_pcie_irq_map()` 按标准 PCI swizzle 规则生成 `interrupt-map`，结合 `interrupt-map-mask = <0x1800 0 0 0x7>`，实现槽位与 INTA~INTD 的轮转映射。

---

## 6. 启动链路：`virt_machine_done()` + `boot.c`

`virt_machine_done()` 位置：`qemu-8.0.2/hw/riscv/virt.c:1238`。

## 6.1 KVM 特殊约束

- KVM 下不支持 machine-mode firmware（除 `-bios none`）
- 若用户没给 `-bios`，代码会强制 `machine->firmware = "none"`

## 6.2 固件/内核选择流程

1. `riscv_find_and_load_firmware(...)`：加载默认 OpenSBI 或用户 firmware
2. 若 `pflash unit1` 存在：
   - `riscv_setup_firmware_boot()` 把 kernel/initrd/cmdline 放入 fw_cfg
   - `kernel_entry` 指向 flash 上半区入口
3. 否则若 `-kernel`：`riscv_load_kernel(...)`
4. 若都没有：`kernel_entry = 0`（fw_dynamic 无 next stage）
5. 若 `pflash unit0` 存在：`start_addr` 改为 flash 基址

## 6.3 FDT 与 reset vector

- `fdt_load_addr = riscv_compute_fdt_addr(...)`
- `riscv_load_fdt(fdt_load_addr, machine->fdt)`
- `riscv_setup_rom_reset_vec(...)` 在 MROM 写入 reset stub 与 fw_dynamic info

KVM 额外：`riscv_setup_direct_kernel(kernel_entry, fdt_load_addr)`。

若 ACPI 开启：`virt_acpi_setup(s)`。

---

## 7. `boot.c` 关键辅助函数（与 virt 强绑定）

文件：`qemu-8.0.2/hw/riscv/boot.c`。

- `riscv_default_firmware_name()`：RV32/RV64 选择不同默认 BIOS 名称
- `riscv_find_firmware()` / `riscv_find_and_load_firmware()`：处理 `-bios` 的 `default/none/自定义`
- `riscv_load_firmware()`：优先 ELF，再原始镜像加载
- `riscv_calc_kernel_start_addr()`：按 2MB（RV64）或 4MB（RV32）对齐
- `riscv_load_kernel()`：支持 ELF/uImage/raw；设置 `bootargs`；可加载 initrd
- `riscv_compute_fdt_addr()`：将 FDT 放在 DRAM 末端向下、2MB 对齐，且兼顾 32 位可寻址约束
- `riscv_setup_rom_reset_vec()`：生成 reset 指令序列，填充 `start_addr`、`fdt_addr`，并附带 OpenSBI fw_dynamic info

---

## 8. ACPI 分支：`virt-acpi-build.c`

触发条件：`virt_is_acpi_enabled(s)`（`acpi != off`）。

## 8.1 构建出的表

- `DSDT`：CPU 设备 + fw_cfg 设备 AML
- `FADT`（rev6, minor 5）
- `MADT`：每 HART 追加 RINTC 项
- `RHCT`：RISC-V Hart Capabilities（含 ISA string node）
- `XSDT` + `RSDP`

构建入口：`virt_acpi_build()`。

## 8.2 暴露给客户机的方式

`virt_acpi_setup()` 通过 `acpi_add_rom_blob()` 将：

- 表数据
- linker 命令 blob
- rsdp

暴露给 Guest，并注册 reset/vmstate：

- `virt_acpi_build_reset()`：复位 patched 状态
- `vmstate_virt_acpi_build`：迁移保存 `patched`

---

## 9. NUMA 与 socket 逻辑（`numa.c`）

`virt` 的 socket/hart/memory 分布高度依赖 `hw/riscv/numa.c`：

- `riscv_socket_count()`：NUMA 开启则返回 node 数，否则 1
- `riscv_socket_first_hartid/last_hartid/hart_count()`：按 node_id 推导 hart 区间
- `riscv_socket_mem_offset/mem_size()`：决定每 socket 的 memory node
- `riscv_socket_fdt_write_id()` 与 `riscv_socket_fdt_write_distance_matrix()`：写 FDT NUMA 属性

这让 `virt` 在 `-numa` 场景下仍保持拓扑与 DT 一致。

---

## 10. 热插拔与 Platform Bus

- `virt_machine_class_init()` 中允许动态 sysbus 设备：
  - `ramfb`
  - `tpm-tis-sysbus`（编译启用 TPM 时）
- `virt_machine_get_hotplug_handler()` 只接管 dynamic sysbus 设备
- `virt_machine_device_plug_cb()` 把热插设备链接到 `platform-bus-device`
- FDT 侧由 `platform_bus_add_all_fdt_nodes()` 自动补节点

这条链路是 `virt` 承载“板上动态 sysbus 外设”的关键机制。

---

## 11. 关键 QEMU API 对照表（按 virt 源码出现频度）

1. **QOM / 对象层**
   - `object_initialize_child`
   - `object_property_set_str/int`
   - `type_register_static` / `type_init`

2. **QDev / SysBus 层**
   - `qdev_new` / `sysbus_realize(_and_unref)`
   - `sysbus_mmio_map` / `sysbus_connect_irq`
   - `qdev_get_gpio_in`

3. **内存映射层**
   - `memory_region_init_ram/rom/alias`
   - `memory_region_add_subregion`
   - `get_system_memory`

4. **FDT 层**
   - `qemu_fdt_add_subnode`
   - `qemu_fdt_setprop_*`
   - `create_device_tree` / `load_device_tree`

5. **启动层**
   - `riscv_find_and_load_firmware`
   - `riscv_load_kernel`
   - `riscv_compute_fdt_addr` / `riscv_load_fdt`
   - `riscv_setup_rom_reset_vec`

---

## 12. 代码行为与文档/实践的几个关键观察

1. `virt.h` 里 `VIRT_CPUS_MAX=512`，而 `docs/system/riscv/virt.rst` 仍写“up to 8 cores”，这在 8.0.2 源码与文档之间存在口径差异。  
2. `virt` 的中断架构可由 machine property 在运行时切换（`aclint`、`aia`、`aia-guests`），FDT 会随之改写。  
3. `virt_machine_done()` 与 `virt_machine_init()`职责分层很清晰：前者做“镜像/FDT/reset 收尾”，后者做“硬件拓扑与设备对象创建”。  
4. `-dtb` 覆盖自动 FDT 后，很多“自动注入属性”（如 rng-seed）由用户 DTB 自行负责。  
5. ACPI 不是独立机器，而是 `virt` 的可选输出路径，与 DT 并行存在。

---

## 13. 建议的阅读与调试顺序（复盘 virt 最快路径）

建议按以下顺序走读：

1. `include/hw/riscv/virt.h`（模型与常量）
2. `hw/riscv/virt.c`
   - `virt_memmap`
   - `virt_machine_init`
   - `create_fdt*`
   - `virt_machine_done`
   - `virt_machine_class_init`
3. `hw/riscv/boot.c`（固件/内核/FDT/reset）
4. `hw/riscv/numa.c`（socket 与 NUMA 映射）
5. `hw/riscv/virt-acpi-build.c`（ACPI 分支）

配套命令：

```bash
# 查看 machine 属性
./output/qemu/bin/qemu-system-riscv64 -M virt,help

# 对比不同中断架构下的 DTB
./output/qemu/bin/qemu-system-riscv64 -M virt,aia=none -machine dumpdtb=virt-none.dtb -nographic
./output/qemu/bin/qemu-system-riscv64 -M virt,aia=aplic -machine dumpdtb=virt-aplic.dtb -nographic
./output/qemu/bin/qemu-system-riscv64 -M virt,aia=aplic-imsic,aia-guests=2 -machine dumpdtb=virt-imsic.dtb -nographic

# 反编译 DTB
dtc -I dtb -O dts virt-imsic.dtb -o virt-imsic.dts
```

---

## 14. 一句话总结

`riscv-virt` 在 QEMU 8.0.2 中是一个“以 HartArray + socket 拓扑为核心、以 FDT/ACPI 双描述输出为接口、可在 PLIC 与 AIA 间切换中断模型、并通过 fw_cfg + reset vector 串联固件/内核启动”的通用虚拟开发板框架。

