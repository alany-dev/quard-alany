#ifndef HW_RISCV_QUARD_STAR__H
#define HW_RISCV_QUARD_STAR__H

#include "hw/riscv/riscv_hart.h"
#include "hw/sysbus.h"
#include "qom/object.h"
#include "hw/block/flash.h"

#define QUARD_STAR_CPUS_MAX 8
#define QUARD_STAR_SOCKETS_MAX 8

#define TYPE_RISCV_QUARD_STAR_MACHINE MACHINE_TYPE_NAME("quard-star")
typedef struct QuardStarState QuardStarState;
DECLARE_INSTANCE_CHECKER(QuardStarState, RISCV_VIRT_MACHINE,
                         TYPE_RISCV_QUARD_STAR_MACHINE)

// xxState 代表一个设备的状态，包含了设备的寄存器、内存映射等信息
struct QuardStarState
{
    /*< private >*/
    MachineState parent;

    /*< public >*/
    RISCVHartArrayState soc[QUARD_STAR_SOCKETS_MAX];
    PFlashCFI01 *flash;
    DeviceState *plic[QUARD_STAR_SOCKETS_MAX];
};

enum
{
    QUARD_STAR_MROM,
    QUARD_STAR_SRAM,
    QUARD_STAR_CLINT,
    QUARD_STAR_PLIC,
    QUARD_STAR_UART0,
    QUARD_STAR_FLASH,
    QUARD_STAR_DRAM,
};

enum
{
    QUARD_STAR_UART0_IRQ = 10, // 定义了串口中断号为10
};

#define QUARD_STAR_PLIC_NUM_SOURCES 127       // PLIC 支持的中断源数量
#define QUARD_STAR_PLIC_NUM_PRIORITIES 7      // PLIC 支持的优先级数量
#define QUARD_STAR_PLIC_PROIORITY_BASE 0x04   // PLIC 中断优先级寄存器的基地址偏移值，用于访问中断优先级信息
#define QUARD_STAR_PLIC_PENDING_BASE 0x1000   // PLIC 中断挂起寄存器的基地址偏移值，用于访问中断挂起状态
#define QUARD_STAR_PLIC_ENABLE_BASE 0x2000    // PLIC 中断使能寄存器的基地址偏移值，用于访问中断使能状态
#define QUARD_STAR_PLIC_ENABLE_STRIDE 0x80    // PLIC 中断使能寄存器之间的地址间隔，用于访问不同上下文的中断使能状态
#define QUARD_STAR_PLIC_CONTEXT_BASE 0x200000 // PLIC 上下文保存寄存器的寄地址偏移值，用于保存中断处理程序的上下文消息
#define QUARD_STAR_PLIC_CONTEXT_STRIDE 0x1000 // PLIC 上下文

#endif