#!/bin/bash
set -e

# 获取当前脚本文件所在的目录
SHELL_FOLDER=$(cd "$(dirname "$0")";pwd)
export PATH=$PATH:$SHELL_FOLDER/riscv64-elf-ubuntu-24.04-gcc/bin

if [ ! -d "$SHELL_FOLDER/output" ]; then  
mkdir $SHELL_FOLDER/output
fi  

echo "------------------------- 编译qemu -------------------------"
cd qemu-8.0.2
if [ ! -d "$SHELL_FOLDER/output/qemu" ]; then  
./configure \
    --prefix=$SHELL_FOLDER/output/qemu \
    --target-list=riscv64-softmmu \
    --enable-gtk \
    --enable-virtfs \
    --disable-gio
fi  
make -j$(( $(nproc) / 2 + 1 ))
make install
cd ..
echo "------------------------- 编译qemu完成 -------------------------"

echo "------------------------- 编译低级引导程序 -------------------------"
CROSS_PREFIX=riscv64-unknown-elf
if [ ! -d "$SHELL_FOLDER/output/lowlevelboot" ]; then  
mkdir $SHELL_FOLDER/output/lowlevelboot
fi  
cd boot
$CROSS_PREFIX-gcc -x assembler-with-cpp -c start.s -o $SHELL_FOLDER/output/lowlevelboot/start.o
$CROSS_PREFIX-gcc -nostartfiles -T./boot.lds -Wl,-Map=$SHELL_FOLDER/output/lowlevelboot/lowlevel_fw.map -Wl,--gc-sections $SHELL_FOLDER/output/lowlevelboot/start.o -o $SHELL_FOLDER/output/lowlevelboot/lowlevel_fw.elf
# 使用gnu工具生成原始的程序bin文件
$CROSS_PREFIX-objcopy -O binary -S $SHELL_FOLDER/output/lowlevelboot/lowlevel_fw.elf $SHELL_FOLDER/output/lowlevelboot/lowlevel_fw.bin
# 使用gnu工具生成反汇编文件，方便调试分析（当然我们这个代码太简单，不是很需要）
$CROSS_PREFIX-objdump --source --demangle --disassemble --reloc --wide $SHELL_FOLDER/output/lowlevelboot/lowlevel_fw.elf > $SHELL_FOLDER/output/lowlevelboot/lowlevel_fw.lst
echo "------------------------- 编译低级引导程序完成 -------------------------"

echo "------------------------- 编译 opensbi -------------------------"
if [ ! -d "$SHELL_FOLDER/output/opensbi" ]; then  
mkdir $SHELL_FOLDER/output/opensbi
fi  
cd $SHELL_FOLDER/opensbi-1.2
# 使用 riscv64-unknown-elf- 交叉编译器；编译 quard_star 平台的 OpenSBI 固件；按照 quard_star 平台配置（objects.mk）生成指定形态的固件（你配置了 FW_JUMP=y）。
make CROSS_COMPILE=$CROSS_PREFIX- PLATFORM=quard_star
cp -r $SHELL_FOLDER/opensbi-1.2/build/platform/quard_star/firmware/*.bin $SHELL_FOLDER/output/opensbi/
echo "------------------------- 编译 opensbi 完成 -------------------------"

echo "------------------------- 生成 sbi.dtb -------------------------"
cd $SHELL_FOLDER/dts
dtc -I dts -O dtb -o $SHELL_FOLDER/output/opensbi/quard_star_sbi.dtb quard_star_sbi.dts
echo "------------------------- 生成 sbi.dtb 完成 -------------------------"

echo "------------------------- 编译 trusted_domain -------------------------"
if [ ! -d "$SHELL_FOLDER/output/trusted_domain" ]; then  
mkdir $SHELL_FOLDER/output/trusted_domain
fi  
cd $SHELL_FOLDER/trusted_domain
$CROSS_PREFIX-gcc -x assembler-with-cpp -c startup.s -o $SHELL_FOLDER/output/trusted_domain/startup.o
$CROSS_PREFIX-gcc -nostartfiles -T./link.lds -Wl,-Map=$SHELL_FOLDER/output/trusted_domain/trusted_fw.map -Wl,--gc-sections $SHELL_FOLDER/output/trusted_domain/startup.o -o $SHELL_FOLDER/output/trusted_domain/trusted_fw.elf
$CROSS_PREFIX-objcopy -O binary -S $SHELL_FOLDER/output/trusted_domain/trusted_fw.elf $SHELL_FOLDER/output/trusted_domain/trusted_fw.bin
$CROSS_PREFIX-objdump --source --demangle --disassemble --reloc --wide $SHELL_FOLDER/output/trusted_domain/trusted_fw.elf > $SHELL_FOLDER/output/trusted_domain/trusted_fw.lst
echo "------------------------- 生成 trusted_fw.bin 完成 -------------------------"

echo "------------------------- 编译 os -------------------------"
if [ ! -d "$SHELL_FOLDER/output/os" ]; then  
mkdir $SHELL_FOLDER/output/os
fi
cd $SHELL_FOLDER/os
make
cp $SHELL_FOLDER/os/os.bin $SHELL_FOLDER/output/os/os.bin
make clean
echo "------------------------- 生成 os.bin 完成 -------------------------"

echo "------------------------- 生成最终固件 fw.bin -------------------------"
if [ ! -d "$SHELL_FOLDER/output/fw" ]; then  
mkdir $SHELL_FOLDER/output/fw
fi  
cd $SHELL_FOLDER/output/fw
rm -rf fw.bin
# 生成一个 32MB 大小、全为 0 的空白固件文件
# dd：Linux下处理二进制文件，of: 输出文件，bs：块大小，count：块的数量，if：输入文件
dd of=fw.bin bs=1k count=32k if=/dev/zero
# # 写入 lowlevel_fw.bin 偏移量地址为 0
dd of=fw.bin bs=1k conv=notrunc seek=0 if=$SHELL_FOLDER/output/lowlevelboot/lowlevel_fw.bin
# 写入 quard_star_sbi.dtb 地址偏移量为 512K，因此 fdt的地址偏移量为 0x80000
dd of=fw.bin bs=1k conv=notrunc seek=512 if=$SHELL_FOLDER/output/opensbi/quard_star_sbi.dtb
# 写入 fw_jump.bin 地址偏移量为 2K*1K= 0x200000，因此 fw_jump.bin的地址偏移量为  0x200000
dd of=fw.bin bs=1k conv=notrunc seek=2k if=$SHELL_FOLDER/output/opensbi/fw_jump.bin
# 写入 trusted_fw.bin 地址偏移量为 1K*4K= 0x400000，因此 trusted_fw.bin的地址偏移量为  0x400000
dd of=fw.bin bs=1k conv=notrunc seek=4K if=$SHELL_FOLDER/output/trusted_domain/trusted_fw.bin
# os.bin 地址偏移量 0x800000
dd of=fw.bin bs=1k conv=notrunc seek=8K if=$SHELL_FOLDER/output/os/os.bin
echo "------------------------- 生成最终固件 fw.bin 完成 -------------------------"
