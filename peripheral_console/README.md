# i.MX6ULL 外设控制台（驱动 + Qt 界面）

基于正点原子 ALPHA V2.2（i.MX6ULL eMMC 版）的一个完整小项目：自己写一个字符设备驱动控制板载 LED、蜂鸣器、按键，改造厂商触摸驱动适配 GT911 触摸屏，再用 Qt 做全屏界面把三路外设和系统状态管起来，开机自启动。

## 一、项目构成

| 模块 | 内容 |
|---|---|
| 字符设备驱动 `chrdevbase.ko` | 一个主设备号 + 三个次设备号，分别对应 LED、蜂鸣器、按键；按键走 GPIO 中断，支持阻塞读与 poll 事件通知，另建 `/proc/chrdevbase_key` 调试节点 |
| 触摸驱动 `gt9xx.ko` | GT911 电容触摸屏驱动，改自厂商源码，适配本板复位脚、中断脚与 I2C 地址 |
| 应用 `console` | Qt Widgets 全屏界面（1024×600）：LED / 蜂鸣器开关、背光滑条、按键计数、温度与内存显示、关机与重启 |
| 设备树 | LCD、GT911、背光、eMMC 与引脚复用配置 |
| 开机脚本 | `S99gt9.sh` 装驱动、建设备节点；`S99zzqt.sh` 关掉文字控制台并启动界面 |

## 二、软硬件环境

| 项 | 版本 / 路径 |
|---|---|
| 开发板 | 正点原子 ALPHA V2.2，i.MX6ULL，eMMC 版 |
| 屏幕 | 7 寸 RGB，1024×600，触摸 IC 为 GT911（I2C-2，7 位地址 0x5d） |
| 内核 | Linux 4.1.15，源码目录 `linux-imx-rel_imx_4.1.15_2.1.0_ga` |
| U-Boot | 2016.03 |
| 交叉编译器 | `gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf`，前缀 `arm-linux-gnueabihf-` |
| Qt | `/opt/fsl-imx-x11/4.1.15-2.1.0`，SDK 环境脚本 `environment-setup-cortexa7hf-neon-poky-linux-gnueabi` |

三样东西必须自己准备，源码包里不含：内核源码树、交叉编译器、Qt SDK。

## 三、目录结构

| 路径 | 内容 |
|---|---|
| `driver/` | 字符设备驱动 `chrdevbase.c`、两个测试程序 `chrdevbaseAPP.c` / `keyAPP.c`、`Makefile` |
| `touch/` | 触摸驱动 `gt9xx.c`、`gt9xx.h`、`Makefile` |
| `qt/` | 界面源码 `main.cpp`、工程文件 `console.pro` |
| `dts/` | 设备树 `imx6ull-alientek-emmc.dts` |
| `boot/` | 开机脚本 `S99gt9.sh`、`S99zzqt.sh` |

## 四、编译

### 1. 字符设备驱动

先确认 `driver/Makefile` 里的 `KERNELDIR` 指向你自己的内核源码目录（默认写的是作者机器上的路径），然后：

```bash
cd driver
make                 # 生成 chrdevbase.ko
make clean           # 清理
```

也可以在命令行临时覆盖，不改文件：

```bash
make KERNELDIR=/path/to/linux-imx-rel_imx_4.1.15_2.1.0_ga
```

### 2. 触摸驱动

```bash
cd touch
make                 # 生成 gt9xx.ko
```

### 3. 两个测试程序

```bash
cd driver
arm-linux-gnueabihf-gcc chrdevbaseAPP.c -o chrdevbaseAPP
arm-linux-gnueabihf-gcc keyAPP.c -o keyAPP
```

### 4. Qt 界面

```bash
cd qt
source /opt/fsl-imx-x11/4.1.15-2.1.0/environment-setup-cortexa7hf-neon-poky-linux-gnueabi
qmake                # 读 console.pro 生成 Makefile
make -j4             # 生成 console
```

改过 `console.pro` 之后必须重新执行 `qmake`，只 `make` 不会生效。

### 5. 设备树

把 `dts/imx6ull-alientek-emmc.dts` 覆盖到内核的 `arch/arm/boot/dts/` 目录下，再编译：

```bash
cd <内核源码目录>
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- imx6ull-alientek-emmc.dtb
```

生成的 `imx6ull-alientek-emmc.dtb` 替换板上引导分区里的同名文件（或随内核一起烧录），触摸节点才会生效。

## 五、部署到板子

| 文件 | 放到板子的位置 |
|---|---|
| `chrdevbase.ko`、`gt9xx.ko` | `/lib/modules/4.1.15/` |
| `console` | `/root/console` |
| `S99gt9.sh` | `/etc/rcS.d/S99gt9.sh`，需要可执行权限 |
| `S99zzqt.sh` | `/etc/rc5.d/S99zzqt.sh`，需要可执行权限 |

手动验证（不用重启）：

```bash
insmod /lib/modules/4.1.15/chrdevbase.ko
cat /proc/devices | grep chrdevbase          # 看到主设备号
mknod /dev/led  c <主设备号> 0
mknod /dev/beep c <主设备号> 1
mknod /dev/key  c <主设备号> 2
chmod 666 /dev/led /dev/beep /dev/key

insmod /lib/modules/4.1.15/gt9xx.ko
EV=`grep -A4 -i goodix /proc/bus/input/devices | grep -o "event[0-9]*" | head -1`
ln -sf /dev/input/$EV /dev/touch

/root/console &
```

卸载顺序与加载相反：

```bash
rmmod gt9xx
rmmod chrdevbase
```

## 六、设备与资源占用

| 设备节点 | 主设备号, 次设备号 | 硬件 | 访问方式 |
|---|---|---|---|
| `/dev/led` | 动态分配（本板 249, 0） | LED，GPIO1_IO03，低电平点亮 | 写 `1` 开灯、`0` 关灯 |
| `/dev/beep` | 249, 1 | 蜂鸣器，GPIO5_IO01，低电平发声 | 写 `b` 响、`B` 停 |
| `/dev/key` | 249, 2 | KEY0，GPIO1_IO18，按下为低 | 阻塞读，读到 `0` 按下、`1` 松开；支持 poll |
| `/dev/touch` | 字符主号 13, 65（event1） | GT911 触摸屏 | 软链接，实际指向 `/dev/input/eventN` |

| 资源 | 编号 |
|---|---|
| 按键中断 | 47（双边沿触发） |
| 触摸中断 | 38 |
| 背光 | `/sys/class/backlight/backlight/brightness`，取值 1–7 |
| 调试节点 | `/proc/chrdevbase_key`，输出 `press / level / led / beep / irq` |

主设备号是动态申请的，换一次加载可能变化，所以开机脚本里用 `grep chrdevbase /proc/devices` 现取，再 `mknod`，不写死。

## 七、常见问题

| 现象 | 原因与处理 |
|---|---|
| `insmod: invalid module format` | 编 ko 用的内核版本或编译器与板上不一致，用板上同一份内核源码重编 |
| `open /dev/led failed` | 节点没建或权限不对，检查 `ls -l /dev/led`，重新 `mknod` + `chmod 666` |
| `gpio_request ... failed` | 引脚已被别的驱动占用，看 `dmesg` 与设备树里的引脚复用 |
| 触摸没反应 | `/dev/touch` 没建出来；先 `ls /dev/input/`、`grep -i goodix /proc/bus/input/devices` 确认驱动是否 probe 成功 |
| 界面起不来、黑屏 | 检查 `QT_QPA_PLATFORM=linuxfb`、Qt 库是否在 rootfs 里，日志见 `/root/zzqt.log` |
| 按键读不到事件 | 中断号配错或引脚复用冲突，`grep -i key0 /proc/interrupts` 确认中断已注册 |
| 关机 / 重启按钮无反应 | 脚本要先卸载两个驱动再关机，日志见 `/tmp/power.log` |

## 八、命令速查

| 命令 | 作用 |
|---|---|
| `lsmod` | 看已加载的模块 |
| `cat /proc/devices` | 看字符设备主号分配 |
| `cat /proc/chrdevbase_key` | 看按键次数、电平、灯与蜂鸣器状态 |
| `ls -l /dev/led /dev/beep /dev/key /dev/touch` | 确认节点与主次号 |
| `cat /proc/interrupts` | 确认中断 47（按键）、38（触摸）已注册 |
| `dmesg \| tail` | 看驱动加载与报错信息 |
| `./chrdevbaseAPP /dev/led 1` | 命令行开灯 |
| `./keyAPP /dev/key` | 命令行读按键 |
| `echo 3 > /sys/class/backlight/backlight/brightness` | 命令行调背光 |
