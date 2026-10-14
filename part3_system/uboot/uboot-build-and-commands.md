# U-Boot 编译、烧写与命令

> 对应实践：U-Boot 初次编译、烧写启动、U-Boot 常用命令。

## 一、U-Boot 简介

U-Boot 是一个 Bootloader（引导加载程序），在系统上电后、Linux 内核运行前执行，主要作用：



* 初始化硬件（CPU、DDR、存储、网络等）；

* 提供命令行，可读写内存、操作存储、网络下载；

* 把 Linux 内核从存储 / 网络加载到内存并启动。

启动链中的位置：**Boot ROM → U-Boot → Linux 内核 → 根文件系统**。

## 二、U-Boot 初次编译



1. 获取并解压源码，进入源码根目录；

2. 修改顶层 Makefile，固定目标架构与交叉编译器：



```
ARCH ?= arm
CROSS_COMPILE ?= arm-linux-gnueabihf-
```

`?=` 为条件赋值，未设置时才生效，也支持命令行临时覆盖；



1. 配置：`make mx6ull_alientek_emmc_defconfig`；

2. 编译：`make -j16`（也可使用自编的一键编译脚本，见 scripts/）。

编译产物：



* `u-boot.bin`：原始二进制镜像；

* `u-boot.imx`：添加了 i.MX 启动头、可直接烧录启动的镜像；

* 两者均为 ARM 架构，可在 i.MX6ULL 运行。

## 三、烧写与启动



* 使用 `imxdownload` 工具把 `u-boot.imx` 烧写到 SD 卡指定扇区；

* 烧写前用 `lsblk` 确认存储设备节点，避免误写主机磁盘；

* 开发板从 SD 卡启动，串口打印 U-Boot 启动日志：


  * CPU 识别为 Freescale i.MX6ULL，内存为 512MB DDR；

  * MMC 识别为 `FSL_SDHC: 0`（SD 卡槽）、`FSL_SDHC: 1`（板载 eMMC）。

* 首次启动提示 `*** Warning - bad CRC, using default environment`：

  原因是 SD 卡中未保存环境变量，U-Boot 使用内置默认值，不影响运行；

  执行 `saveenv` 保存后，重启警告消失。

## 四、常用命令



| 类别   | 命令                            | 作用               |
| ---- | ----------------------------- | ---------------- |
| 信息查询 | bdinfo / version              | 查看硬件参数、版本        |
| 环境变量 | printenv / setenv / saveenv   | 查看 / 修改 / 保存环境变量 |
| 内存   | md / mm / mw / nm / cp        | 查看 / 修改 / 拷贝内存   |
| MMC  | mmc list / mmc dev / mmc info | 列出、切换、查看存储设备     |
| 网络   | ping / tftpboot / nfs         | 连通测试、下载文件        |
| 启动   | bootz / bootm                 | 启动内核             |

实测：配置正确 IP 后 `ping` 主机显示 `host 192.168.1.100 is alive`；`tftpboot` 可从 Ubuntu TFTP 服务器下载文件到指定内存。

## 五、遇到的问题与解决



* `ARCH` 误写为 `aem`，导致编译失败，检查拼写后解决；

* `imxdownload` 未放到对应目录，报权限 / 命令错误，放对位置后正常；

* Windows 换行符（CRLF）导致 shell 脚本执行异常，转换为 LF 后正常。