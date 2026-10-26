# Linux 内核移植

> 对应实践：在 NXP 官方内核基础上，完成自己开发板的移植 —— 配置编译环境、编译官方内核、添加自己的开发板（配置文件 + 设备树）、修改网络驱动（SMSC PHY 复位），最终编译出能在自己板子上启动的内核。
> 核心链路：
> **修改 Makefile → 配置内核 → 添加开发板 → 修改驱动 → make 编译 → 启动验证**
> 核心链路：
>
> **修改 Makefile → 配置内核 → 添加开发板 → 修改驱动 → make 编译 → 启动验证**



***

## 一、工程准备

用 VSCode 打开内核源码目录即可。常用目录：`arch`（架构）、`drivers`（驱动）、`init`（内核启动）、`fs`（文件系统）。



***

## 二、编译 NXP 官方内核（重点）

先用官方默认配置把内核编译出来、跑起来，**验证整条编译链路没问题**，再移植自己的板子。

### 1. 顶层 Makefile：指定架构和编译器



```
ARCH ?= arm
CROSS_COMPILE ?= arm-linux-gnueabihf-
```



* 交叉编译 = 在 x86 电脑上编译 ARM 程序，必须指定交叉工具链前缀。

* `?=`：命令行没传该变量时才用默认值，传了以命令行优先。

### 2. 配置并编译



```
make imx_v7_defconfig   # ① 载入 NXP 官方默认配置，生成 .config
make menuconfig         # ② 图形化配置界面（可勾选/取消功能，可选步骤）
make -j16               # ③ 编译，-j16 = 16 核并行
```

产物位置要记住：内核镜像 `arch/arm/boot/zImage`、设备树 `arch/arm/boot/dts/*.dtb`。

配置传递链：`defconfig` → `.config`（menuconfig 可改）→ 编译，每次 make 都读 `.config`。

### 3. 启动测试（U-Boot + TFTP）



```
setenv ipaddr 192.168.6.251            # 开发板 IP
setenv serverip 192.168.6.250          # Ubuntu（TFTP 服务器）IP
tftp 80800000 zImage                   # 内核下载到内存 0x80800000
tftp 83000000 imx6ull-alientek-emmc.dtb  # 设备树下载到 0x83000000
bootz 80800000 - 83000000              # 启动内核，中间 - 表示无 ramdisk
```

内核地址和 dtb 地址要分开（0x80800000 / 0x83000000），防止互相覆盖。

### 4. 常见坑：根文件系统缺失（重点记）



```
VFS: Unable to mount root fs on unknown-block(179,2)
Kernel panic - not syncing: VFS: Unable to mount root fs
```



* **原因**：bootargs 里 `root=` 指定的根文件系统不存在或没挂载成功。

* **解决**：改 bootargs，如 SD 卡根文件系统：`root=/dev/mmcblk1p2 rootwait rw`。

* **核心理解**：内核只是系统核心，用户程序都在根文件系统里；找不到 rootfs 就无法切到用户态，只能 panic。`rootwait` = 等 SD/EMMC 设备就绪再挂载，防止 "设备没初始化完就去挂载"。



***

## 三、添加自己的开发板（重点）

官方内核默认只支持 NXP 官方板。要让内核认识我们的板子，必须做两件事：



1. **默认配置文件**（defconfig，`arch/arm/configs/`）→ 管 "编译哪些功能"

2. **设备树文件**（dts，`arch/arm/boot/dts/`）→ 管 "板上有哪些硬件"



```
cp arch/arm/configs/imx_v7_defconfig arch/arm/configs/mx6ull_alientek_emmc_defconfig
cp arch/arm/boot/dts/imx6ull-14x14-evk.dts arch/arm/boot/dts/imx6ull-alientek-emmc.dts
```



* dts = 设备树源码（文本）；dtsi = 公共部分（类似头文件）；dtb = dts 编译后的二进制，内核启动实际使用。

* 设备树本质是 "**用树形结构描述板子硬件**"（CPU、内存、外设分别是什么、挂在哪、用哪个驱动），内核不再把硬件写死在代码里。

* **修改关键点**：`model` / `compatible` 改成自己板子的名字（内核靠 `compatible` 匹配 machine）；外设节点按实际硬件改。

* **别忘了**：新 dts 要加进 `arch/arm/boot/dts/Makefile`，否则 make 不生成它的 dtb。

编译测试：



```
make mx6ull_alientek_emmc_defconfig
make -j16
```

下载新 zImage、dtb 能正常启动 = 开发板添加成功。



***

## 四、次要配置（了解即可）



* **CPU 主频**：在设备树 `operating-points`（频率电压表）里改，改完测稳定性，贪高易死机。知道 "在哪改、怎么验证" 即可。

* **8 线 EMMC**：设备树 usdhc 节点改 `bus-width = <8>`（前提硬件真接了 8 根线，否则读写出错）。

* **保存配置**：`make savedefconfig` 生成精简 defconfig 后拷回 `arch/arm/configs/`，这是保存内核配置的标准做法。



***

## 五、修改网络驱动（本章最重要，重点记）

### 1. 背景：为什么必须改

正点原子板的 PHY 从 **KSZ8081 换成了 LAN8720A**（SMSC 公司），且**复位 IO 也不同**，官方驱动带不动。网口 = **FEC**（MAC 控制器，IMX6ULL 内部自带，NXP 官方驱动没问题）+ **LAN8720A**（PHY 芯片，管物理层信号）。

> 排查思路：NFS 挂载老失败 → 怀疑网络驱动 → 网络驱动分 MAC + PHY → MAC 官方正常 → 官方板 PHY 是 KSZ8081、我们是 LAN8720A → 
>
> **问题锁定在 LAN8720A 的 PHY 驱动**
>
> 。

完整流程 6 步：**改设备树（复位引脚、时钟引脚、PHY 地址）→ 改 fec\_main.c → menuconfig 使能 → 改 smsc.c → 测试**。

### 2. 设备树：复位引脚和时钟引脚

文件：`arch/arm/boot/dts/imx6ull-alientek-emmc.dts`

**① 释放被 SPI4 占用的复位引脚**：ENET1\_RST 接 `SNVS_TAMPER7`、ENET2\_RST 接 `SNVS_TAMPER8`，默认被 SPI4 占用必须释放 —— 删 `pinctrl_spi4` 中对应两行，删 `spi4` 节点中 `pinctrl-assert-gpios`（GPIO5\_IO08）和 `cs-gpios`（GPIO5\_IO07）两行。

**② 在&#x20;**`iomuxc_snvs`**&#x20;下添加复位引脚组**（`pinctrl_xxx` = 引脚配置组，把 TAMPER 脚配成 GPIO 作复位脚；`0x10B0` = 上拉输出电气属性，不用记）：



```
&iomuxc_snvs {
    imx6ul-evk {
        pinctrl_enet1_reset: enet1resetgrp {
            fsl,pins = <MX6ULL_PAD_SNVS_TAMPER7_GPIO5_IO07 0x10B0>;
        };
        pinctrl_enet2_reset: enet2resetgrp {
            fsl,pins = <MX6ULL_PAD_SNVS_TAMPER8_GPIO5_IO08 0x10B0>;
        };
    };
};
```

**③ 改网络时钟引脚电气属性**（原来 `0x4001b031`，改成 `0x4001b009`，让引脚既能输出时钟又能回读，LAN8720A 需要）：



```
MX6UL_PAD_ENET1_TX_CLK__ENET1_REF_CLK1   0x4001b009
MX6UL_PAD_ENET2_TX_CLK__ENET2_REF_CLK2   0x4001b009
```

### 3. 设备树：fec 节点和 PHY 地址

**① fec1/fec2 的&#x20;**`pinctrl-0`**&#x20;加上复位引脚组；② 加复位参数**（后面 smsc.c 驱动要读这两个属性）：



```
&fec1 {
    pinctrl-0 = <&pinctrl_enet1 &pinctrl_enet1_reset>;
    phy-reset-gpios = <&gpio5 7 GPIO_ACTIVE_LOW>;  /* 复位引脚 GPIO5_IO07，低电平有效 */
    phy-reset-duration = <200>;                    /* 复位低电平持续 200ms */
};
&fec2 {
    pinctrl-0 = <&pinctrl_enet2 &pinctrl_enet2_reset>;
    phy-reset-gpios = <&gpio5 8 GPIO_ACTIVE_LOW>;
    phy-reset-duration = <200>;
};
```

**③ 改 PHY 地址**：PHY 地址 = CPU 通过 MDIO 总线找到 PHY 的编号，**ENET1 的 LAN8720A 地址为 0x0，ENET2 的为 0x1**：



```
mdio {
    ethphy0: ethernet-phy@0 {                 /* ENET1：地址 0 */
        compatible = "ethernet-phy-ieee802.3-c22";
        smsc,disable-energy-detect;           /* 声明是 SMSC PHY，内核用 smsc.c 驱动 */
        reg = <0>;
    };
    ethphy1: ethernet-phy@1 {                 /* ENET2：地址 1 */
        compatible = "ethernet-phy-ieee802.3-c22";
        smsc,disable-energy-detect;
        reg = <1>;
    };
};
```



* `@` 后的数字 = PHY 地址，必须和 `reg = <x>` 一致。

* `smsc,disable-energy-detect` = 声明 PHY 是 SMSC 公司产品（关闭节能检测），内核才会用 smsc.c。

改完设备树用 `make dtbs`（或 `make -j16`）重新编译。

### 4. 修改 fec\_main.c（了解）

文件 `drivers/net/ethernet/freescale/fec_main.c`，在 `fec_probe` 中用 `ioremap`（物理地址映射成虚拟地址）+ `writel` 设置 TX\_CLK 复用寄存器的 **SION 位**（Software Input ON），让时钟引脚能回读时钟信号。知道作用即可，具体地址不用记。

### 5. menuconfig 使能 SMSC PHY 驱动



```
-> Device Drivers
   -> Network device support
      -> PHY Device support and infrastructure
         -> Drivers for SMSC PHYs   <*>   ← 选 * 编译进内核（不是模块）
```

### 6. 修改 smsc.c 驱动（重点）

文件 `drivers/net/phy/smsc.c`。**为什么要改**：参考 U-Boot 经验 ——U-Boot 中需给 LAN8720A 做一次软复位（设置 BMCR 寄存器 bit5 = 1），推断 Linux 中也需软复位。原函数只在 PHY 处于 **Powerdown** 时才软复位，改为**每次调用都主动拉低复位引脚 → 延时 → 拉高，并做软复位**：



```
static int smsc_phy_reset(struct phy_device *phydev)
{
    int err, phy_reset;
    int msec = 1;
    struct device_node *np;
    int timeout = 50000;

    if (phydev->addr == 0) /* FEC1 */
        np = of_find_node_by_path("/soc/aips-bus@02100000/ethernet@02188000");
    else /* FEC2 */
        np = of_find_node_by_path("/soc/aips-bus@02000000/ethernet@020b4000");
    if (np == NULL) return -EINVAL;

    err = of_property_read_u32(np, "phy-reset-duration", &msec);
    if (!err && msec > 1000) msec = 1;          /* 复位时长不超过 1s */
    phy_reset = of_get_named_gpio(np, "phy-reset-gpios", 0);
    if (!gpio_is_valid(phy_reset)) return;

    gpio_direction_output(phy_reset, 0);
    gpio_set_value(phy_reset, 0);
    msleep(msec);                               /* 保持低电平 200ms */
    gpio_set_value(phy_reset, 1);

    /* 软复位：读特殊模式寄存器，判断是否处于 Powerdown */
    int rc = phy_read(phydev, MII_LAN83C185_SPECIAL_MODES);
    if (rc < 0) return rc;
    if ((rc & MII_LAN83C185_MODE_MASK) == MII_LAN83C185_MODE_POWERDOWN) {
        rc |= MII_LAN83C185_MODE_ALL;
        phy_write(phydev, MII_LAN83C185_SPECIAL_MODES, rc);
        phy_write(phydev, MII_BMCR, BMCR_RESET);
        do {
            udelay(10);
            if (timeout-- == 0) return -1;
            rc = phy_read(phydev, MII_BMCR);
        } while (rc & BMCR_RESET);
    }
    return 0;
}
```

关键点（重点记）：



* `of_find_node_by_path`：按设备树路径找 FEC1/FEC2 节点（of = Open Firmware，设备树 API 前缀）。

* `of_property_read_u32(np, "phy-reset-duration", &msec)`：读复位时长（我们配的 200）。

* `of_get_named_gpio(np, "phy-reset-gpios", 0)`：读复位 GPIO。

* `gpio_direction_output / gpio_set_value`：拉低 → `msleep(msec)` 保持 200ms → 拉高。

* **核心改动**：把 "只在 Powerdown 时才软复位" 改为 "每次调用都执行复位"，保证 LAN8720A 稳定。

最后在 smsc.c 文件头补两个头文件（用到了 GPIO 函数）：



```
#include <linux/of_gpio.h>
#include <linux/io.h>
```

### 7. 网卡驱动测试



```
ifconfig          # 看当前活动的网卡
ifconfig -a       # 看所有网卡：eth0、eth1 都在（eth0 对应 ENET2，eth1 对应 ENET1）
ifconfig eth0 up  # 打开网卡
ifconfig eth1 up
```

成功日志（关键验证点）：



```
fec 20b40000.ethernet eth0: Freescale FEC PHY driver [SMSC LAN8710/LAN8720] (mii_bus:phy_addr=...)
IPv6: ADDRCONF(NETDEV_CHANGE): eth1: link becomes ready
```



* 看到 **SMSC LAN8710/LAN8720** = SMSC 驱动成功匹配 PHY。

* 看到 **link becomes ready** = 链路建立成功，网络驱动调试完成。

之后 `ifconfig eth0 ``192.168.6.251`` up` 配 IP、`ping` 主机即可。



***

## 六、本章小结



1. **编译环境**：顶层 Makefile 配 `ARCH` / `CROSS_COMPILE` → `make xxx_defconfig` → `make -j16`。

2. **添加开发板**：`arch/arm/configs/` 加 defconfig（管编译）+ `arch/arm/boot/dts/` 加 dts（管硬件）。

3. **改硬件配置**：CPU 主频、EMMC 8 线都在设备树里改，编译进 dtb。

4. **改驱动**：PHY 复位延时不够导致 link up 失败 → 改 `drivers/net/phy/smsc.c` 复位时序解决。

5. **验证**：TFTP 下载 zImage/dtb → bootz 启动 → 看 `Link is Up` → `ifconfig` / `ping`。

**一句话概括**：配置（Makefile/defconfig）→ 描述硬件（设备树）→ 修驱动（smsc.c）→ 编译 → 启动验证。



***

## 附：源码位置速查



| 内容          | 路径                                                |
| ----------- | ------------------------------------------------- |
| 顶层 Makefile | `Makefile`                                        |
| 开发板默认配置     | `arch/arm/configs/mx6ull_alientek_emmc_defconfig` |
| 开发板设备树      | `arch/arm/boot/dts/imx6ull-alientek-emmc.dts`     |
| SMSC PHY 驱动 | `drivers/net/phy/smsc.c`                          |
| 内核镜像产物      | `arch/arm/boot/zImage`                            |