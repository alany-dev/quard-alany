// syscall.c
#include "stddef.h"
#include "stdint.h"
#include "stdio.h"

// 系统调用函数：id=系统调用号，arg1/arg2/arg3=三个参数，返回系统调用结果
size_t syscall(size_t id, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3)
{
    long ret;  // 存储系统调用的返回值
    // 内嵌汇编（asm volatile）：保证汇编代码不被编译器优化删除/重排
    asm volatile(
        // 以下是RISC-V架构的系统调用寄存器约定：
        "mv a7, %1\n\t" // 将系统调用号id放到a7寄存器（RISC-V约定syscall id存在a7）
        "mv a0, %2\n\t" // 将参数arg1放到a0寄存器（第一个参数）
        "mv a1, %3\n\t" // 将参数arg2放到a1寄存器（第二个参数）
        "mv a2, %4\n\t" // 将参数arg3放到a2寄存器（第三个参数）
        "ecall\n\t"     // 触发RISC-V的环境调用指令，进入内核态执行系统调用
        "mv %0, a0"     // 系统调用完成后，将a0寄存器（返回值）赋值给ret变量
        // 输出操作数：%0对应ret，"=r"表示写入通用寄存器，结果存到ret
        : "=r"(ret)
        // 输入操作数：%1=id, %2=arg1, %3=arg2, %4=arg3；"r"表示用通用寄存器传值
        : "r"(id), "r"(arg1), "r"(arg2), "r"(arg3)
        // 破坏描述符：告诉编译器这些寄存器被修改，避免优化出错
        : "a7", "a0", "a1", "a2", "memory");
    return ret; // 返回系统调用的结果
}