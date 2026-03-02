#ifndef __CONTEXT_H__
#define __CONTEXT_H__

#include "os.h"

/*S模式的trap上下文*/
typedef struct pt_regs {
    reg_t x0;
    reg_t ra;
    reg_t sp; // 栈指针（U模式用 UserStack，S模式用 KernelStack）
    reg_t gp;
    reg_t tp;
    reg_t t0;
    reg_t t1;
    reg_t t2;
    reg_t s0;
    reg_t s1;
    reg_t a0;
    reg_t a1;
    reg_t a2;
    reg_t a3;
    reg_t a4;
    reg_t a5;
    reg_t a6;
    reg_t a7;
    reg_t s2;
    reg_t s3;
    reg_t s4;
    reg_t s5;
    reg_t s6;
    reg_t s7;
    reg_t s8;
    reg_t s9;
    reg_t s10;
    reg_t s11;
    reg_t t3;
    reg_t t4;
    reg_t t5;
    reg_t t6;
    /* S模式下的寄存器 */
    reg_t sstatus; // CPU 状态寄存器（控制特权级、中断使能等）
    reg_t sepc;    // 异常返回地址（sret 跳转用）
} pt_regs;

#endif