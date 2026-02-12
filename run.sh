#!/bin/bash
# 获取脚本所在的绝对路径
SHELL_FOLDER=$(cd "$(dirname "$0")";pwd)

# 启动QEMU RISC-V64（无图形界面版）
$SHELL_FOLDER/output/qemu/bin/qemu-system-riscv64 \
-M quard-star \
-monitor stdio \
-m 1G \
-smp 8
