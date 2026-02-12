#!/bin/bash
# 获取脚本所在的绝对路径
SHELL_FOLDER=$(cd "$(dirname "$0")";pwd)

# 启动QEMU RISC-V64（无图形界面版）
$SHELL_FOLDER/output/qemu/bin/qemu-system-riscv64 \
-M quard-star \
-m 1G \
-smp 8 \
-bios none \
-drive if=pflash,bus=0,unit=0,format=raw,file=$SHELL_FOLDER/output/lowlevelboot/fw.bin \
-nographic \
--parallel none