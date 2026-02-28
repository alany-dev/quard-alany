# quard-alany
从零基于 qemu 创建 riscv 嵌入式开发板，并移植 FreeRTOS

# 快速开始
```bash
# 依赖安装
sudo apt update && sudo apt install ninja-build pkg-config libglib2.0-dev libpixman-1-dev libgtk-3-dev libcap-ng-dev libattr1-dev libsdl2-dev device-tree-compiler bison flex gperf intltool mtd-utils libslirp-dev -y

# 编译/运行
./build.sh  # 编译
./run.sh    # 运行
```


# 参考项目链接
- [基于qemu-riscv从0开始构建嵌入式linux系统](https://quard-star-tutorial.readthedocs.io/zh-cn/latest/ch0.html)
- [rCore](https://rcore-os.cn/rCore-Tutorial-Book-v3/chapter0/index.html)
- [quard-star](https://gitee.com/yang_lian/quard-star)