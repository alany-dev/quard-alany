
# 编译环境
```bash
sudo apt install ninja-build pkg-config libglib2.0-dev libpixman-1-dev libgtk-3-dev libcap-ng-dev libattr1-dev libsdl2-dev device-tree-compiler bison flex gperf intltool mtd-utils libslirp-dev
```

源码下载安装
```bash
tar -xvJf qemu-8.0.2.tar.xz
cd qemu
./configure
make -j2
sudo make install 

qemu-img -V # 查看版本信息

qemu-system-riscv64 # 启动

# 如果是 腾讯云，可以 控制台，VNC 登录查看 qemu 界面。
```