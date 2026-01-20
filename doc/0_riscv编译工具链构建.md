# 下载二进制文件构建
进入 [riscv-gnu-toolchain](https://github.com/riscv-collab/riscv-gnu-toolchain) 的 github 仓库 tags，找到合适的 ubuntu 版本，以及 riscv 32/64 位版本。  
当前 riscv 文件夹下已有 `riscv64-elf-ubuntu-24.04-gcc.tar.xz`，解压后，把 bin 目录添加到 `.bashrc` 即可使用。
```bash
export PATH=$PATH:/home/ubuntu/projects/quard-alany/riscv64-elf-ubuntu-24.04-gcc/bin
```
> 本项目使用的编译器为 `riscv64-unknown-elf-gcc`

> riscv64-linux-gnu-gcc  
链接 glibc（GNU C 库），提供完整的 POSIX 兼容接口，支持多线程、动态链接、标准 I/O 等功能，适合复杂的用户态应用开发。  
riscv64-unknown-elf-gcc  
默认链接 Newlib（轻量级嵌入式 C 库），仅提供基础的 C 标准库功能，无系统调用依赖，适合资源受限的嵌入式场景。