.macro loop, cunt       // 定义宏，简单的 CPU 空转延时
    li      t1, 0xffff  // t1 = 65535 (内层循环计数)
    li      t2, \cunt   // t2 = 传入的参数 (外层循环计数)
1:                      // 标签 1
    nop
    addi    t1, t1, -1  // t1 自减
    bne     t1, x0, 1b  // 如果 t1 != 0，跳转回标签 1 (backward)
    li      t1, 0xffff  // 重置内层循环
    addi    t2, t2, -1  // t2 自减
    bne     t2, x0, 1b  // 如果 t2 != 0，跳转回标签 1
.endm

.macro load_data, _src_start, _dst_start, _dst_end  // memcpy 汇编版，把 Flash 数据拷贝到 RAM
    bgeu    \_dst_start, \_dst_end, 2f  // 如果 目标起始 >= 目标结束，直接结束
1:
    lw      t0, (\_src_start)           // 从源地址加载一个字(4字节)到 t0
    sw      t0, (\_dst_start)           // 把 t0 写入目标地址
    addi    \_src_start, \_src_start, 4 // 源地址 + 4
    addi    \_dst_start, \_dst_start, 4 // 目标地址 + 4
    bltu    \_dst_start, \_dst_end, 1b  // 如果 目标当前 < 目标结束，继续循环
2:
.endm

	.section .text
	.globl _start
	.type _start,@function

// 这是 CPU 上电后执行的第一段逻辑
_start:
    // 计算源地址 (Flash): 0x202 << 20 = 0x20200000
    li      a0, 0x202
    slli    a0, a0, 20      
    // 计算目的地址 (DRAM): 0x800 << 20 = 0x80000000
    li      a1, 0x800
    slli    a1, a1, 20      
    // 计算结束地址 (DRAM): 0x802 << 20 = 0x80200000 (拷贝大小 2MB)
    li      a2, 0x802
    slli    a2, a2, 20      
    
    // 执行拷贝：Flash(0x20200000) -> DRAM(0x80000000)
    load_data a0,a1,a2

    // 计算源地址 (Flash): 0x2008 << 16 = 0x20080000
    li      a0, 0x2008
    slli    a0, a0, 16       
    // 计算目的地址 (DRAM): 0x822 << 20 = 0x82200000
    li      a1, 0x822
    slli    a1, a1, 20       
    // 计算结束地址
    li      a2, 0x8228
    slli    a2, a2, 16       

    // 执行拷贝：Flash(0x20080000) -> DRAM(0x82200000)
    load_data a0,a1,a2

    csrr    a0, mhartid         // 读取当前 CPU 核心 ID
    li      t0, 0x0      
    beq     a0, t0, _no_wait    // 如果是 0 号核 (主核)，直接跳转
    loop    0x1000              // 如果是 从核，执行延时循环
_no_wait:

    // 准备参数 a1 = 设备树在内存中的地址 (OpenSBI 约定的传参规则)
    li      a1, 0x822
    slli    a1, a1, 20       // a1 = 0x82200000

    // 准备跳转目标 t0 = OpenSBI 在内存中的入口地址
    li      t0, 0x800
    slli    t0, t0, 20       // t0 = 0x80000000
    
    // 跳过去！并将控制权移交给 OpenSBI
    jr      t0

.end