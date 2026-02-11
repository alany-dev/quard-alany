# 获取当前脚本文件所在的目录
SHELL_FOLDER=$(cd "$(dirname "$0")";pwd)

echo "------------------------- 编译qemu---------------------------------------"
cd qemu-8.0.2
if [ ! -d "$SHELL_FOLDER/output/qemu" ]; then  
./configure \
    --prefix=$SHELL_FOLDER/output/qemu \
    --target-list=riscv64-softmmu \
    --enable-virtfs \
    --disable-gio \
    --disable-gtk \
    --disable-sdl \
    --disable-opengl \
    --disable-vte
fi  
make -j$(( $(nproc) / 2 + 1 ))
make install
cd ..