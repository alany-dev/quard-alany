# quard_star Flash 添加流程与原理详解（小白友好版）

## 1. 你这次到底改了什么？（先有全局图）

你这次改动可以理解成 5 件事：

1. **让构建系统知道 quard_star 需要 pflash 设备能力**  
   文件：`qemu-8.0.2/hw/riscv/Kconfig:89`

2. **在板级状态结构体里加一个 Flash 句柄**  
   文件：`qemu-8.0.2/include/hw/riscv/quard_star.h:25`

3. **在板级内存地图里预留 Flash 地址空间**  
   文件：`qemu-8.0.2/hw/riscv/quard_star.c:36`

4. **实现 `quard_star_flash_create()`：创建 + 配置 + realize + 挂载 Flash**  
   文件：`qemu-8.0.2/hw/riscv/quard_star.c:124`

5. **修改 reset 跳转目标到 Flash 基址，并在 machine init 里调用 flash create**  
   文件：`qemu-8.0.2/hw/riscv/quard_star.c:117`、`qemu-8.0.2/hw/riscv/quard_star.c:168`

一句话：你已经把 “Flash 设备对象” 真正接进了 `quard_star` 板子的硬件模型。

---

## 2. 先建立直觉：QEMU 里的“设备”是什么？

可以把 QEMU 想成一个“软件电路板工厂”：

- `DeviceState`：一颗“还没上电”的芯片对象
- `qdev_prop_set_*`：给芯片焊配置（总线宽度、ID、容量参数）
- `sysbus_realize`：芯片上电，进入可工作状态
- `memory_region_add_subregion`：把芯片的地址空间贴到 SoC 总线地址图

所以你的 Flash 接入，本质是：

**新建一颗 pflash 芯片 → 配参数 → 上电 → 挂到 0x20000000。**

---

## 3. 分步骤详解（从配置到运行）

## 步骤 A：打开依赖能力

你在 `Kconfig` 增加了：

```kconfig
config QUARD_STAR
    bool
    select SERIAL
    select PFLASH_CFI01
```

### 原理

`TYPE_PFLASH_CFI01` 对应的设备实现，只有在 `PFLASH_CFI01` 组件被纳入构建时才可用。  
如果这里不 `select`，可能出现编译通过但链接缺设备、或运行时机型缺组件等问题。

---

## 步骤 B：在板级状态里保存 Flash 对象

你在 `quard_star.h` 新增：

```c
PFlashCFI01 *flash;
```

### 原理

这是“板子私有状态中的设备句柄”。之后你要：

- 绑定 `-drive if=pflash`
- 访问或扩展 Flash 行为

都需要保存这个指针。

> 注意：`PFlashCFI01` 是类型名；`PFLASH_CFI01(dev)` 是转换宏。两者不能混用。

---

## 步骤 C：把 Flash 放进物理地址图

你新增了：

```c
[QUARD_STAR_FLASH] = {0x20000000, 0x2000000}
```

即：

- 基址：`0x20000000`
- 大小：`0x02000000`（32 MiB）

### 原理

CPU 访问某设备，靠的是“物理地址命中”到这个设备的 MMIO 区间。  
没有这张映射表，CPU 根本找不到这颗 Flash。

---

## 步骤 D：创建 Flash 设备对象（最关键）

`quard_star_flash_create()` 核心流程如下。

### D.1 创建设备对象

```c
DeviceState *dev = qdev_new(TYPE_PFLASH_CFI01);
```

这是创建“CFI pflash01 芯片实例”。此时只是对象存在，还没真正工作。

### D.2 配置属性（你设置得很标准）

```c
qdev_prop_set_uint64(dev, "sector-length", 256 * KiB);
qdev_prop_set_uint8(dev, "width", 4);
qdev_prop_set_uint8(dev, "device-width", 2);
qdev_prop_set_bit(dev, "big-endian", false);
qdev_prop_set_uint16(dev, "id0", 0x89);
qdev_prop_set_uint16(dev, "id1", 0x18);
...
```

这些参数可以这样理解：

- `sector-length`：擦除粒度（最小擦除单位）
- `width=4`：对外总线 32-bit
- `device-width=2`：内部按 16-bit 芯片组织，拼成 32-bit
- `id0~id3`：CFI 查询时返回的制造商/器件 ID

### D.3 把设备挂到 QOM 树，并暴露 `pflash0` 别名

```c
object_property_add_child(OBJECT(s), "quard-star.flash0", OBJECT(dev));
object_property_add_alias(OBJECT(s), "pflash0", OBJECT(dev), "drive");
```

这一步让板级对象能“看到”这颗设备，同时给命令行 legacy 绑定提供桥接。

### D.4 连接 `-drive if=pflash`

```c
s->flash = PFLASH_CFI01(dev);
pflash_cfi01_legacy_drive(s->flash, drive_get(IF_PFLASH, 0, 0));
```

### 原理

- `drive_get(IF_PFLASH, 0, 0)`：找命令行里的 pflash 单元 0
- `pflash_cfi01_legacy_drive(...)`：把后端镜像文件绑定到设备

如果你后续用：

```bash
-drive if=pflash,format=raw,file=flash0.bin
```

这一步就是让 `flash0.bin` 真正成为这颗芯片的“内容”。

### D.5 计算块数并 realize

```c
qdev_prop_set_uint32(dev, "num-blocks", flashsize / sector_size);
sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
```

这里把“总容量”翻译成“块数量”。realize 后，设备才具备 MMIO region。

### D.6 挂入系统地址空间

```c
memory_region_add_subregion(system_memory, flashbase,
    sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0));
```

这就是最后一锤：把 Flash 的第 0 号 MMIO 区域映射到 `0x20000000`。

---

## 步骤 E：让 CPU 复位后跳去 Flash

你把 `riscv_setup_rom_reset_vec()` 的 `start_addr` 改成了 Flash 基址：

```c
riscv_setup_rom_reset_vec(...,
    quard_star_memmap[QUARD_STAR_FLASH].base,
    ...)
```

### 原理

MROM 里的 reset stub 相当于“第一跳引导程序”。  
你现在让它第一跳进 Flash 开头，而不是以前的 MROM 地址。

因此：

- 有有效 Flash 引导内容：会继续启动
- 没有有效内容：会卡住或抛异常（常见是非法指令）

---

## 4. 对照 `virt` 板，你的方案是否合理？

是合理的。`virt` 板在 `qemu-8.0.2/hw/riscv/virt.c` 里也是同样套路：

- `qdev_new(TYPE_PFLASH_CFI01)`
- `qdev_prop_set_*` 配参数
- `pflash_cfi01_legacy_drive(...)` 绑定镜像
- `sysbus_realize_and_unref(...)`
- `memory_region_add_subregion(...)`

你的 `quard_star` 就是在复用这条成熟路径。

---

## 5. 小白最容易踩的坑（你现在就能规避）

1. **类型名写错**  
   `PFlashCFI01 *flash;` 才是字段声明；`PFLASH_CFI01(...)` 只能用于转换。

2. **Flash 容量必须与扇区对齐**  
   你已经用 `assert(QEMU_IS_ALIGNED(...))` 做了保护，这很好。

3. **跳转目标改到 Flash 后，必须有可执行内容**  
   否则 reset 后会“跳进空白区”。

4. **目前 DRAM 大小仅 `0x80`（128 字节）**  
   这对真正 boot 来说几乎不可用，后续建议改为 `machine->ram_size`。

5. **还没生成 FDT 里的 Flash 节点**  
   设备可访问不等于 OS 可发现；要给内核友好识别，后续需补 FDT 描述。

---

## 6. 你现在可以怎么验证（最小验证）

```bash
# 编译
sudo ./build.sh

# 启动
./output/qemu/bin/qemu-system-riscv64 \
  -M quard-star \
  -nographic \
  -m 1G -smp 8 \
  -drive if=pflash,format=raw,file=flash0.bin
```

建议加调试：

```bash
-d guest_errors,unimp
```

若启动异常，优先检查：

- Flash 镜像是否有有效入口代码
- reset vector `start_addr` 是否与你镜像布局一致
- 地址是否与内存映射冲突

---

## 7. 下一步建议（从“能跑”到“好用”）

1. 把 DRAM 从固定 `0x80` 改成 `machine->ram_size`。  
2. 增加 `create_fdt_flash()`，把 Flash 节点写进 DTB。  
3. 规划 Flash 布局（bootloader 区 / 固件区 / 参数区）。  
4. 明确 reset 跳转协议（跳地址、镜像格式、是否 OpenSBI）。  
5. 若要兼容传统命令行，可支持双 flash bank（`pflash0/pflash1`）。

---

## 8. 一句话总结

你这次改动已经把 `quard_star` 的 Flash 从“概念地址”升级为“真实设备”：

- 构建依赖有了
- 板级状态有了
- MMIO 映射有了
- 命令行后端绑定有了
- 复位跳转也接上了

现在的核心任务是：把“Flash 可访问”推进到“Flash 可启动 + OS 可识别”。

