CC=riscv64-unknown-elf-gcc

write:test_write.c syscall.c
	$(CC) $^ -o write.out


# qemu-riscv64 write.out 
# Hello, RISC-V!
# ret:15