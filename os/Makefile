
CROSS_COMPILE = riscv64-unknown-elf-
CFLAGS = -nostdlib -fno-builtin -mcmodel=medany

# riscv64-unknown-elf-gcc 工具链可以同时编译汇编和 C 代码
CC = ${CROSS_COMPILE}gcc
OBJCOPY = ${CROSS_COMPILE}objcopy
OBJDUMP = ${CROSS_COMPILE}objdump

SRCS_ASM = \
	entry.S \
	kerneltrap.S

SRCS_C = \
	sbi.c \
	printf.c \
	batch.c \
	trap.c \
	main.c

# 将源文件替换为 .o 文件
OBJS = $(SRCS_ASM:.S=.o)
OBJS += $(SRCS_C:.c=.o)

os.elf: ${OBJS}
# 指定 os.ld 链接脚本，输出 os.elf，$^ 自动替换为所有依赖文件
	${CC} ${CFLAGS} -T os.ld -Wl,-Map=os.map -o os.elf $^  
# os.elf -> os.bin 纯二进制文件
	${OBJCOPY} -O binary os.elf os.bin

# %通配符，任意字符串
# .c 文件 编译成 .o 文件   $@ 替换为目标文件名  $< 替换为第一个依赖文件名
%.o : %.c
	${CC} ${CFLAGS} -c -o $@ $<

# .S 汇编文件 编译为 .o 文件
%.o : %.S
	${CC} ${CFLAGS} -c -o $@ $<


.PHONY : clean
clean:
	rm -rf *.o *.bin *.elf
