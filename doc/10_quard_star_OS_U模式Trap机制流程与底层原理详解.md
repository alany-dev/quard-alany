# quard_star OS U 模式 Trap 机制流程与底层原理详解

## 1. 文档目标

本文基于仓库当前 `os/` 代码，系统总结你已实现的 **U 模式 Trap 机制**，重点回答三个问题：

1. 从 S 模式如何进入 U 模式执行用户代码。
2. U 模式触发 trap（当前是 `ecall`）后，硬件和软件分别做了什么。
3. Trap 上下文如何保存、交给 C 处理、以及如何返回（设计路径与当前状态）。

> 说明：本文描述“当前代码已经具备的机制”与“当前版本仍待补齐的环节”都会明确标注，避免和理想实现混淆。

---

## 2. 相关代码与职责分工

当前实现集中在以下文件：

1. `os/main.c`：内核入口，启动 U 模式上下文初始化。
2. `os/batch.c`：构造首个用户任务上下文，调用 `__restore` 下沉到 U 模式。
3. `os/trap.c`：设置 `stvec`，并在 `trap_handler` 中读取/打印 trap 信息。
4. `os/kerneltrap.S`：Trap 汇编入口 `__alltraps` 和恢复出口 `__restore`。
5. `os/context.h`：`pt_regs` 结构定义（Trap 上下文内存布局）。
6. `os/riscv.h`：CSR 读写封装（`scause/sepc/sstatus/stvec` 等）。

---

## 3. 总体执行时序（从启动到 trap）

### 3.1 阶段 A：S 模式启动

执行链：

1. `entry.S::_start` 设置启动栈 `sp=boot_stack_top`。
2. 跳转到 `os_main()`。
3. `os_main()` 调用 `app_init_context()`。

### 3.2 阶段 B：构造首个 U 模式现场

`app_init_context()` 主要做四件事：

1. 准备用户栈顶：`user_sp = UserStack + USER_STACK_SIZE`。
2. 安装 trap 入口：`trap_init()` -> `stvec = __alltraps`。
3. 准备用户入口 PC：`tasks.sepc = testsys`。
4. 在内核栈顶伪造一个 `pt_regs`，写入：
   - `cx_ptr->sepc = tasks.sepc`
   - `cx_ptr->sstatus = tasks.sstatus`
   - `cx_ptr->sp = tasks.sp`（用户栈）

最后调用 `__restore(cx_ptr)`，把这份“伪造好的 trap 上下文”当作恢复源，直接 `sret` 进入 U 模式。

### 3.3 阶段 C：U 模式执行并触发 trap

当前用户代码是 `testsys()`，其中执行：

1. `syscall(2,3,4,5)`。
2. 内联汇编把参数写入 `a7/a0/a1/a2`。
3. 执行 `ecall`。

`ecall` 触发后，CPU 从 U -> S，进入 `stvec` 指向的 `__alltraps`。

### 3.4 阶段 D：Trap 入口保存现场并进入 C 处理

`__alltraps` 的关键路径：

1. `csrrw sp, sscratch, sp`：交换内核栈与用户栈指针。
2. `addi sp, sp, -34*8`：在内核栈上分配 `pt_regs`。
3. 保存通用寄存器、`sstatus`、`sepc`，并把用户 `sp` 写入上下文。
4. `a0 = sp`，调用 `trap_handler(pt_regs *cx)`。

`trap_handler` 当前会打印：

1. `scause`
2. `a0/a1/a2/a7`
3. `sepc/sstatus/sp`

随后进入死循环（当前版本用于验证 trap 机制已触发）。

---

## 4. 底层原理拆解

## 4.1 `sret` 如何决定返回到 U 还是 S

`sret` 的目标特权级由 `sstatus.SPP` 决定：

1. `SPP=0`：返回 U 模式。
2. `SPP=1`：返回 S 模式。

因此，从 `__restore` 进入用户态时，`pt_regs.sstatus` 中的 SPP 位必须为 0。

---

## 4.2 U 模式 `ecall` 触发后，硬件自动动作

当 U 模式执行 `ecall`，硬件至少会完成：

1. `sepc <- faulting_pc`（记录触发点地址）。
2. `scause <- 8`（User-mode environment call）。
3. `sstatus.SPP <- 0`（记录陷入前来自 U）。
4. `pc <- stvec`（跳转到 S 模式 trap 向量）。

注意：RISC-V 不会自动切栈，所以软件必须自己从用户栈切到内核栈。你当前通过 `sscratch + csrrw` 完成了这一点。

---

## 4.3 为什么 `sscratch` 方案是关键

你当前设计中：

1. 进入 U 前，`__restore` 把 `pt_regs.sp`（用户栈）写入 `sscratch`。
2. 同时把当前内核栈保留在寄存器交换路径中。
3. Trap 进入第一条指令就 `csrrw sp, sscratch, sp`，立即得到内核栈。

这套方案的本质是：**用 `sscratch` 作为跨特权级切栈的“单寄存器桥接点”**，避免 trap 入口在用户栈上继续运行。

---

## 4.4 `pt_regs` 与汇编保存偏移的一致性

`pt_regs` 共 34 个 `reg_t`：

1. `x0..x31`（按结构体字段顺序映射）
2. `sstatus`
3. `sepc`

汇编中分配 `34*8` 字节，并按固定槽位读写：

1. `2*8(sp)` 保存用户态 `sp`
2. `32*8(sp)` 保存 `sstatus`
3. `33*8(sp)` 保存 `sepc`

这与 `context.h` 的结构定义是对齐的，因此 C 层 `trap_handler(pt_regs *cx)` 能直接按字段访问。

---

## 4.5 Trap 返回路径的设计原理

设计上，`trap_handler` 返回一个 `pt_regs*` 给 `__restore`：

1. `__restore` 从该上下文恢复 CSR 与 GPR。
2. 释放 trap 栈帧。
3. 再次 `csrrw sp, sscratch, sp` 切回用户栈。
4. `sret` 返回到 `sepc`。

这意味着你已经具备“可扩展到调度/信号/线程切换”的基础接口：只要 C 层返回不同的 `pt_regs*`，就能切换到不同任务。

---

## 5. 当前实现已达成的能力

基于现有代码，可以确认以下能力已实现：

1. 能从 S 模式正确进入 U 模式执行用户函数。
2. 能捕获 U 模式 `ecall` 并进入统一 trap 入口。
3. 能完成 U/S 栈切换、寄存器现场保存、CSR 保存。
4. C 层可读取 trap 原因和 syscall 参数寄存器，说明上下文传递正确。
5. 已具备返回框架（`__restore`），但当前被 `trap_handler` 死循环阻断。

---

## 6. 关键寄存器语义速查

1. `stvec`：S 模式 trap 向量基址（你设置为 `__alltraps`）。
2. `sscratch`：软件保留 CSR，这里承担“U/S 栈交换中转”角色。
3. `scause`：trap 原因，U `ecall` 为 `8`。
4. `sepc`：trap 返回地址（`sret` 使用）。
5. `sstatus.SPP`：`sret` 返回目标特权级选择位。

---

## 7. 当前版本的边界与待完善点（非常关键）

以下是和“完整可运行 syscall 返回”相比的当前差异：

1. `trap_handler` 目前死循环，未走返回路径。
2. 未在处理 `ecall` 后执行 `cx->sepc += 4`，若直接返回会反复陷入同一条 `ecall`。
3. `app_init_context()` 中 `sstatus &= (0U << 8)` 会把 `sstatus` 清零（表达式结果为 0），建议改为“清除 SPP 位而非清空整个 CSR”。
4. 尚未做 `scause` 分发（仅打印），例如：
   - `8`：U `ecall`
   - 页故障/非法指令等异常分支
5. 未建立 syscall 表与返回值回填规则（如 `cx->a0 = ret`）。

> 结论：你的 trap 基础框架（入口、切栈、上下文、出口）已经搭好，当前处于“验证触发与上下文正确性”的阶段，离“可返回、可分发、可扩展 syscall”只差 C 层控制流与少量 CSR/PC 细节处理。

---

## 8. 一张图看完整链路

```text
S-mode(_start/os_main)
    |
    | app_init_context()
    |   - stvec = __alltraps
    |   - build pt_regs on KernelStack
    |   - sepc = testsys, sp = UserStackTop
    v
__restore(pt_regs*)
    |
    | restore regs/csrs
    | csrrw sp, sscratch, sp   (switch to user stack)
    | sret
    v
U-mode(testsys -> ecall)
    |
    | hardware trap:
    |   sepc/scause/sstatus updated
    |   pc = stvec
    v
__alltraps
    |
    | csrrw sp, sscratch, sp   (switch to kernel stack)
    | save GPR + sepc + sstatus into pt_regs
    | call trap_handler(cx)
    v
trap_handler (C)
    |
    | print debug info
    | (current: while(1))
    v
(future) return cx -> __restore -> sret -> back to U
```

---

## 9. 小结

你现在的 U 模式 trap 机制，已经完成了最难的底座部分：

1. 正确的特权级进入/退出路径（`sret` + SPP 语义）。
2. 正确的 U/S 栈切换机制（`sscratch` + `csrrw`）。
3. 完整的 trap 上下文内存化（`pt_regs`）与 C 层可观测性。

接下来只要把 `trap_handler` 从“打印并停机”升级为“分发并返回”，这套机制就能演进为可用的用户态系统调用框架。
