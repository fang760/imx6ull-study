# Linux 内核编译与顶层 Makefile

> 对应实践：内核获取、初次编译、工程目录分析、内核 Makefile。

## 一、内核源码获取与准备

- 从正点原子资料获取 i.MX6ULL 对应内核源码（linux-imx-4.1.15）；
- 解压：`tar -vxf linux-imx-4.1.15-*.tar.bz2`；
- 安装编译依赖：`sudo apt-get install lzop`（否则缺少 lzop 报错）。

## 二、内核源码目录结构

| 目录 | 作用 |
|---|---|
| arch/ | 架构相关代码（ARM 在 arch/arm） |
| block/ | 块设备相关 |
| drivers/ | 设备驱动 |
| fs/ | 文件系统 |
| include/ | 头文件 |
| init/ | 内核启动初始化（start_kernel 等） |
| kernel/ | 内核核心代码 |
| mm/ | 内存管理 |
| net/ | 网络协议 |
| scripts/ | 编译脚本 |

## 三、配置与编译

```bash
# 配置（生成 .config）
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- imx_v7_defconfig
# 编译（16 线程）
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -j16
```

编译完成输出 `Kernel: arch/arm/boot/zImage is ready`。

产物：

- `arch/arm/boot/zImage`：最终可烧录的压缩内核镜像；
- `arch/arm/boot/dts/*.dtb`：设备树文件，描述硬件外设信息。

## 四、编译问题：yylloc 重复定义

现象：首次编译时 `scripts/dtc/dtc` 报 `multiple definition of 'yylloc'`。

原因：系统自带 gcc 版本较高（gcc 10 及以上默认启用 `-fno-common`），与 4.1.15 老内核不兼容。

解决：在顶层 Makefile 的 `HOSTCFLAGS` 定义行末尾添加 `-fcommon`，恢复旧版 common 符号处理行为，重新编译即可。

## 五、关键概念

- `vmlinux`：编译出的未压缩 ELF 内核；
- `zImage`：压缩后的内核镜像，可烧录；
- `.dtb`：由设备树源文件 `.dts` 编译得到的设备树二进制。
