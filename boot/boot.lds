OUTPUT_ARCH( "riscv" )  /* 指定目标架构为 RISC-V */
ENTRY( _start )         /* 指定程序的入口点标签为 _start */

MEMORY                  /* 内存布局定义 */
{ 
    /* 定义名为 flash 的区域：
       ORIGIN = 0x20000000:这正是 QEMU pflash 的起始地址
       LENGTH = 512k: 这个引导程序最大不能超过 512KB
    */
    flash (rxai!w) : ORIGIN = 0x20000000, LENGTH = 512k 
}

SECTIONS                /* 段定义 */
{
  .text :               /* 代码段 */
  {
    KEEP(*(.text))      /* 强制保留所有代码，防止被链接器优化掉 */
  } >flash              /* 将代码段放入上面定义的 flash 区域 */
}