# U-Boot 源码目录与顶层 Makefile

> 对应实践：U-Boot 工程目录分析、VSCode 工程、make xxx_defconfig 与 make 过程。

## 一、U-Boot 源码目录结构

| 目录 | 作用 |
|---|---|
| arch/ | 各 CPU 架构相关代码（ARM 在 arch/arm） |
| board/ | 各开发板的板级支持代码 |
| cmd/ | 命令行所支持命令的实现 |
| common/ | 核心框架通用代码 |
| configs/ | 各开发板默认配置文件（xxx_defconfig） |
| drivers/ | 各类外设驱动 |
| dts/ | 设备树源文件 |
| fs/ | 文件系统支持 |
| include/ | 头文件 |
| lib/ | 通用库函数 |
| net/ | 网络协议支持 |
| scripts/ | 编译脚本与构建工具 |
| tools/ | 编译辅助工具 |

根目录编译产物：`u-boot.bin`（纯二进制）、`u-boot.imx`（带 i.MX 头部）、`u-boot.lds`（链接脚本）、`u-boot.map`（符号映射表）。

## 二、make xxx_defconfig 过程

执行 `make mx6ull_alientek_emmc_defconfig` 后：

1. `%defconfig` 模式规则匹配目标，进入配置流程；
2. 编译 `scripts/basic` 下的 `fixdep` 等基础工具；
3. 编译 `scripts/kconfig` 配置工具；
4. kconfig 读取 `configs/xxx_defconfig`，在根目录生成 `.config`。

`.config` 中关键项：`CONFIG_SYS_ARCH="arm"`、`CONFIG_SYS_CPU`、`CONFIG_SYS_BOARD`、`CONFIG_SYS_VENDOR`、`CONFIG_SYS_SOC`；
`ARCH := $(CONFIG_SYS_ARCH:"%"=%)` 会把 `"arm"` 去掉引号得到 `arm`。

## 三、make 编译过程

1. 读取 `.config`，根据 `CONFIG_xxx` 决定编译的目录与文件；
2. 条件编译 `ALL-$(CONFIG_XXX) += 文件名`：值为 `y` 时加入，`n` 时跳过；
3. `Makefile.build` 通过 `%.o:%.c`、`%.o:%.S` 递归编译子目录；
4. 用 `ld -r` 可重定位链接，把每个目录的 `.o` 打包成 `built-in.o`；
5. `patsubst %/, %/built-in.o, $(libs-y)` 把目录名替换为 built-in.o 路径；
6. 收集各目录 built-in.o 与启动入口，按 `u-boot.lds` 链接，生成 u-boot、u-boot.bin、u-boot.imx。

## 四、Makefile 关键语法

- 变量赋值：`=`（延时）、`:=`（立即）、`+=`（追加）、`?=`（条件赋值）；
- 变量引用：`$(VAR)`；
- 模式规则：`%.o:%.c`；
- 自动变量：`$@`（目标）、`$<`（第一个依赖）、`$^`（全部依赖）；
- 文本函数：`patsubst`（模式替换）；
- 伪目标：`.PHONY`。
