#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/guest-random.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/char/serial.h"
#include "target/riscv/cpu.h"

#include "hw/riscv/riscv_hart.h"
#include "hw/riscv/quard_star.h"
#include "hw/riscv/boot.h"
#include "hw/riscv/numa.h"
#include "hw/intc/riscv_aclint.h"
#include "hw/intc/riscv_aplic.h"
#include "hw/intc/sifive_plic.h"

#include "chardev/char.h"
#include "sysemu/device_tree.h"
#include "sysemu/sysemu.h"
#include "sysemu/kvm.h"
#include "sysemu/tpm.h"

/*
typedef struct MemMapEntry {
    hwaddr base;  // 内存映射的起始地址
    hwaddr size;  // 内存映射的大小
} MemMapEntry;
*/
static const MemMapEntry quard_star_memmap[] = {
    [QUARD_STAR_MROM]  = {       0x0,    0x8000}, // ROM
    [QUARD_STAR_SRAM]  = {    0x8000,    0x8000}, // CPU 高速缓存
    [QUARD_STAR_CLINT] = {0x02000000,   0x10000}, // CLINT 定时器和软件中断控制器
    [QUARD_STAR_PLIC]  = {0x0c000000, 0x4000000}, // PLIC 中断控制器
    [QUARD_STAR_UART0] = {0x10000000,     0x100}, // UART0 串口
    [QUARD_STAR_FLASH] = {0x20000000, 0x2000000}, // Flash 存储
    [QUARD_STAR_DRAM]  = {0x80000000,      0x80}, // DRAM 内存
};

// 创建CPU
static void quard_star_cpu_create(MachineState *machine)
{
    int i, base_hartid, hart_count;
    char *soc_name;
    QuardStarState *s = RISCV_VIRT_MACHINE(machine);

    if (QUARD_STAR_SOCKETS_MAX < riscv_socket_count(machine)) {
        error_report("number of sockets/nodes should be less than %d",
                     QUARD_STAR_SOCKETS_MAX);
        exit(1);
    }

    for (i = 0; i < riscv_socket_count(machine); i++) {
        if (!riscv_socket_check_hartids(machine, i)) {
            error_report("discontinuous hartids in socket%d", i);
            exit(1);
        }

        base_hartid = riscv_socket_first_hartid(machine, i);
        if (base_hartid < 0) {
            error_report("can't find hartid base for socket%d", i);
            exit(1);
        }

        hart_count = riscv_socket_hart_count(machine, i);
        if (hart_count < 0) {
            error_report("can't find hart count for socket%d", i);
            exit(1);
        }

        soc_name = g_strdup_printf("soc%d", i);
        object_initialize_child(OBJECT(machine), soc_name, &s->soc[i],
                                TYPE_RISCV_HART_ARRAY);
        g_free(soc_name);
        object_property_set_str(OBJECT(&s->soc[i]), "cpu-type",
                                machine->cpu_type, &error_abort);
        object_property_set_int(OBJECT(&s->soc[i]), "hartid-base",
                                base_hartid, &error_abort);
        object_property_set_int(OBJECT(&s->soc[i]), "num-harts",
                                hart_count, &error_abort);
        sysbus_realize(SYS_BUS_DEVICE(&s->soc[i]), &error_abort);
    }
}

// 创建内存
static void quard_star_memory_create(MachineState *machine)
{
    QuardStarState *s           = RISCV_VIRT_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();
    // 分配三片存储空间 dram sram mrom
    MemoryRegion *dram_mem = g_new(MemoryRegion, 1); // DRAM
    MemoryRegion *sram_mem = g_new(MemoryRegion, 1); // SRAM
    MemoryRegion *mask_rom = g_new(MemoryRegion, 1); // MROM

    memory_region_init_ram(dram_mem, NULL, "riscv_quard_star_board.dram",
                           quard_star_memmap[QUARD_STAR_DRAM].size, &error_fatal);
    memory_region_add_subregion(system_memory,
                                quard_star_memmap[QUARD_STAR_DRAM].base, dram_mem);

    memory_region_init_ram(sram_mem, NULL, "riscv_quard_star_board.sram",
                           quard_star_memmap[QUARD_STAR_SRAM].size, &error_fatal);
    memory_region_add_subregion(system_memory,
                                quard_star_memmap[QUARD_STAR_SRAM].base, sram_mem);

    memory_region_init_rom(mask_rom, NULL, "riscv_quard_star_board.mrom",
                           quard_star_memmap[QUARD_STAR_MROM].size, &error_fatal);
    memory_region_add_subregion(system_memory,
                                quard_star_memmap[QUARD_STAR_MROM].base, mask_rom);

    // MROM（Mask ROM）中生成一段可以在系统复位（Reset）时执行的机器码（Bootloader Stub/Reset Vector）
    riscv_setup_rom_reset_vec(machine, &s->soc[0],
                              quard_star_memmap[QUARD_STAR_FLASH].base, // 目标跳转地址
                              quard_star_memmap[QUARD_STAR_MROM].base,  // ROM起始地址
                              quard_star_memmap[QUARD_STAR_MROM].size,  // ROM大小
                              0x0,                                      // kernel_entry
                              0x0);                                     // fdt_load_addr
}

static void quard_star_flash_create(MachineState *machine)
{
#define QUARD_STAR_FLASH_SECTOR_SIZE (256 * KiB)               // 0x40000
    QuardStarState *s           = RISCV_VIRT_MACHINE(machine); //
    MemoryRegion *system_memory = get_system_memory();         // 获取全局的系统内存根区域，稍后会将 Flash 挂载到这里。
    DeviceState *dev            = qdev_new(TYPE_PFLASH_CFI01); // 创建一个新的设备对象，类型为 CFI01 标准的并行 Flash。此时设备已创建但尚未初始化（未 realize）。

    qdev_prop_set_uint64(dev, "sector-length", QUARD_STAR_FLASH_SECTOR_SIZE); // Flash 扇区大小，擦除和写入的最小单位
    qdev_prop_set_uint8(dev, "width", 4);                                     // 总线宽度 4字节（32位），表示每次访问可以读写4字节数据
    qdev_prop_set_uint8(dev, "device-width", 2);                              // 单芯片宽度 2 字节。QEMU 模拟两片 16 位的 Flash 芯片组成一个 32 位的设备
    qdev_prop_set_bit(dev, "big-endian", false);                              // 小端模式，和 RISC-V 架构一致
    qdev_prop_set_uint16(dev, "id0", 0x89);                                   // 制造商与设备 ID (CFI 查询响应)
    qdev_prop_set_uint16(dev, "id1", 0x18);
    qdev_prop_set_uint16(dev, "id2", 0x00);
    qdev_prop_set_uint16(dev, "id3", 0x00);
    qdev_prop_set_string(dev, "name", "quard-star.flash0");

    object_property_add_child(OBJECT(s), "quard-star.flash0", OBJECT(dev)); // 将 Flash 设备作为 QuardStarState 的子对象，属性名为 "quard-star.flash0"。QOM Tree记录。
    object_property_add_alias(OBJECT(s), "pflash0", OBJECT(dev), "drive");  // 添加别名 "pflash0"，指向 Flash 设备的 "drive" 属性，方便在其他地方通过 "pflash0" 来访问 Flash 设备的驱动信息

    s->flash = PFLASH_CFI01(dev);
    pflash_cfi01_legacy_drive(s->flash, drive_get(IF_PFLASH, 0, 0)); // 处理旧式的驱动连接方式，确保如果有对应的 -drive 参数，它会被正确连接到这个 Flash 设备上。

    hwaddr flashsize = quard_star_memmap[QUARD_STAR_FLASH].size; // 从板级的内存映射表中获取 Flash 的预定大小和基地址。
    hwaddr flashbase = quard_star_memmap[QUARD_STAR_FLASH].base;

    assert(QEMU_IS_ALIGNED(flashsize, QUARD_STAR_FLASH_SECTOR_SIZE));                  // 确保 Flash 总大小是扇区大小的整数倍（必须对齐）。
    assert(flashsize / QUARD_STAR_FLASH_SECTOR_SIZE <= UINT32_MAX);                    // 确保块数量没有溢出。
    qdev_prop_set_uint32(dev, "num-blocks", flashsize / QUARD_STAR_FLASH_SECTOR_SIZE); // 计算总块数（总大小 / 扇区大小）并设置给设备。这是 Flash 大小的最终决定参数。
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);                       // 关键步骤。这会触发设备的 realize 方法，完成设备的内部初始化。如果失败（error_fatal），QEMU 会直接报错退出。

    // sysbus_mmio_get_region(...) 获取 Flash 设备提供的第 0 号内存区域（即 Flash 的存储空间）。
    // memory_region_add_subregion 将这块内存区域“粘贴”到系统内存空间的 flashbase 地址处。
    memory_region_add_subregion(system_memory, flashbase, sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0));
}

// 创建 PLIC 中断控制器
static void quard_star_plic_create(MachineState *machine)
{
    int socket_count  = riscv_socket_count(machine);
    QuardStarState *s = RISCV_VIRT_MACHINE(machine);
    int i, hart_count, base_hartid;
    for (i = 0; i < socket_count; i++) {
        hart_count  = riscv_socket_hart_count(machine, i);
        base_hartid = riscv_socket_first_hartid(machine, i);
        char *plic_hart_config;

        plic_hart_config = riscv_plic_hart_config_string(machine->smp.cpus);

        // 保留 plic 指针
        // 机器中的其他设备（如串口 UART、VirtIO 设备、PCIe 控制器等）产生的硬件中断，都需要连接到 PLIC 的特定引脚（IRQ lines）上。
        s->plic[i] = sifive_plic_create(quard_star_memmap[QUARD_STAR_PLIC].base + i * quard_star_memmap[QUARD_STAR_PLIC].size, // PLIC 基地址偏移
                                        plic_hart_config, hart_count, base_hartid,
                                        QUARD_STAR_PLIC_NUM_SOURCES,    // PLIC 支持的中断源数量
                                        QUARD_STAR_PLIC_NUM_PRIORITIES, // PLIC 支持的优先级数量
                                        QUARD_STAR_PLIC_PROIORITY_BASE, // PLIC 中断优先级
                                        QUARD_STAR_PLIC_PENDING_BASE,   // PLIC 中断挂起
                                        QUARD_STAR_PLIC_ENABLE_BASE,    // PLIC 中断使能
                                        QUARD_STAR_PLIC_ENABLE_STRIDE,  // PLIC 中断使能寄存
                                        QUARD_STAR_PLIC_CONTEXT_BASE,   // PLIC 上下文保存寄存器
                                        QUARD_STAR_PLIC_CONTEXT_STRIDE, // PLIC 上下文保存寄存器间隔
                                        quard_star_memmap[QUARD_STAR_PLIC].size);
        g_free(plic_hart_config);
    }
}

// 创建 ACLINT 定时器和软件中断控制器
static void quard_star_aclint_create(MachineState *machine)
{
    int i, hart_count, base_hartid;
    int socket_count = riscv_socket_count(machine);

    for (i = 0; i < socket_count; i++) {
        hart_count  = riscv_socket_hart_count(machine, i);
        base_hartid = riscv_socket_first_hartid(machine, i);

        // SWI (Software Interrupts / 软件中断)，实现 IPI 核间中断。
        // 最后一个参数 sswi 设置为 false，表示这是一个标准的 MSWI（Machine Software Interrupt），而不是 SSWI（Supervisor Software Interrupt）。
        // 这意味着软件中断将直接连接到每个 CPU 的 Machine 模式软件中断输入，而不是 Supervisor 模式的软件中断输入。
        // MSWI 固件 OpenSBI 使用，最高权限。M 机器模式
        // SSWI 操作系统 OS 使用，次高权限。S 监督模式
        riscv_aclint_swi_create(quard_star_memmap[QUARD_STAR_CLINT].base + i * quard_star_memmap[QUARD_STAR_CLINT].size, // CLINT 基地址偏移
                                base_hartid,
                                hart_count,
                                false);

        // MTIMER (Machine Timer / 机器定时器)，实现 系统时基 和 定时器中断。
        riscv_aclint_mtimer_create(quard_star_memmap[QUARD_STAR_CLINT].base + i * quard_star_memmap[QUARD_STAR_CLINT].size + RISCV_ACLINT_SWI_SIZE, // CLINT 基地址偏移
                                   RISCV_ACLINT_DEFAULT_MTIMER_SIZE,
                                   base_hartid,
                                   hart_count,
                                   RISCV_ACLINT_DEFAULT_MTIMECMP,
                                   RISCV_ACLINT_DEFAULT_MTIME,
                                   RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ,
                                   true);
    }
}

// quard-star 初始化各种硬件
static void quard_star_machine_init(MachineState *machine)
{
    //  创建CPU
    quard_star_cpu_create(machine);
    //  创建主存
    quard_star_memory_create(machine);
    // 创建 Flash 存储
    quard_star_flash_create(machine);
    // 创建 PLIC 中断控制器
    quard_star_plic_create(machine);
    // 创建 RISCV_ACLINT
    quard_star_aclint_create(machine);
}

static void quard_star_machine_instance_init(Object *obj)
{
}

// 初始化 machine class
static void quard_star_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc                        = "RISC-V Quard Star board";
    mc->init                        = quard_star_machine_init;
    mc->max_cpus                    = QUARD_STAR_CPUS_MAX;
    mc->default_cpu_type            = TYPE_RISCV_CPU_BASE;
    mc->pci_allow_0_address         = true;
    mc->possible_cpu_arch_ids       = riscv_numa_possible_cpu_arch_ids;
    mc->cpu_index_to_instance_props = riscv_numa_cpu_index_to_props;
    mc->get_default_cpu_node_id     = riscv_numa_get_default_cpu_node_id;
    mc->numa_mem_supported          = true;
}

// 注册 quard-star
static const TypeInfo quard_star_machine_typeinfo = {
    .name          = MACHINE_TYPE_NAME("quard-star"),
    .parent        = TYPE_MACHINE,
    .class_init    = quard_star_machine_class_init,
    .instance_init = quard_star_machine_instance_init,
    .instance_size = sizeof(QuardStarState),
    .interfaces    = (InterfaceInfo[]){
                                       {TYPE_HOTPLUG_HANDLER},
                                       {}},
};

static void quard_star_machine_init_register_types(void)
{
    type_register_static(&quard_star_machine_typeinfo);
}

type_init(quard_star_machine_init_register_types)