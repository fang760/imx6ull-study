#!/bin/sh
# ==============================================================================
#  /etc/rcS.d/S99gt9.sh  ——  开机自动加载驱动的脚本
# ==============================================================================
#  为什么放在 /etc/rcS.d/：
#    SysV init 启动时先由 /etc/inittab 的 si::sysinit:/etc/init.d/rcS 跑
#    /etc/rcS.d/ 里的脚本（系统初始化阶段），再进入运行级目录 rc5.d。
#    驱动必须在 UI 之前就装好，因为界面打开 /dev/led 时设备节点得已经存在。
#    文件名规则：S = Start（启动时执行），数字越小越早，S99 = 最后执行。
#
#  这个脚本干三件事：
#    1) 装 chrdevbase.ko（LED/蜂鸣器/KEY0 字符设备驱动，1 主设备号 + 3 次设备号）
#    2) 从 /proc/devices 读出内核动态分配的主设备号，创建设备节点
#       /dev/led（灯）、/dev/beep（蜂鸣器）、/dev/key（按键）
#    3) 装 gt9xx.ko（GT911 触摸驱动），并打印调试信息到日志
#
#  日志：/root/gt9_boot.log（开机出问题时先看这个文件）
# ==============================================================================

LOG=/root/gt9_boot.log                                  # 所有输出都追加到这里

{
  echo "=== GT9 BOOT $(date) ==="                            # 在日志里打一条时间分隔线，方便分段查看每次开机记录

  # ① 装字符设备驱动：insmod 后会执行模块的 chrdevbase_init()
  #    里面申请 GPIO、注册中断、alloc_chrdev_region，并在 dmesg 打印 major/irq
  insmod /lib/modules/4.1.15/chrdevbase.ko                  # 加载字符设备驱动，开局的第一件事

  # ② 取主设备号：/proc/devices 的格式是 "249 chrdevbase"，
  #    awk 取第 2 列等于 chrdevbase 的那一行的第 1 列（本板是 249）
  #    为什么不写死 249：因为驱动用的是动态分配，换内核/换顺序号会变
  MAJ=`awk '$2=="chrdevbase"{print $1}' /proc/devices`      # 从已注册清单里抓出本次分到的主设备号

  if [ -n "$MAJ" ]; then                                    # 只有抓到号才建节点，防驱动加载失败时建出错节点
    # 一个主设备号 + 三个次设备号，所以下面建三个节点，次设备号依次是 0/1/2
    rm -f /dev/led /dev/beep /dev/key                         # 先删旧的，避免残留错误节点
    rm -f /dev/chrdevbase /dev/chrdevbase1 /dev/chrdevbase2    # 顺手清掉旧命名的节点，免得两套名字并存
    # mknod 创建设备节点：c 表示字符设备，$MAJ 主设备号，后面是次设备号
    mknod /dev/led  c "$MAJ" 0                                 # 次设备号 0 → LED
    mknod /dev/beep c "$MAJ" 1                                 # 次设备号 1 → 蜂鸣器
    mknod /dev/key  c "$MAJ" 2                                 # 次设备号 2 → KEY0 按键
    chmod 666 /dev/led /dev/beep /dev/key                      # 给所有用户读写权限，Qt 才能打开
  fi                                                        # 判断结束

  ls -l /dev/led /dev/beep /dev/key                            # 确认三个节点都建出来了

  # ③ 装触摸驱动：probe 会解析设备树、探测 I2C 地址、注册 /dev/input/event1
  insmod /lib/modules/4.1.15/gt9xx.ko                       # 加载 GT911 触摸驱动，probe 里会申请中断
  sleep 1                                               # 等一下让 probe 跑完

  # 4) 给触摸起一个固定好认的名字：事件号是系统按注册顺序给的，换环境可能变，
  #    所以这里现查一次，再建软链接；界面里统一用 /dev/touch，不用关心今天是 event 几
  EV=`grep -A4 -i goodix /proc/bus/input/devices | grep -o "event[0-9]*" | head -1`
  if [ -n "$EV" ]; then
    ln -sf /dev/input/$EV /dev/touch
  fi
  ls -l /dev/touch

  dmesg | tail -8                                       # 驱动打印的初始化信息
  ls -l /dev/input/                                     # 确认 event1 存在
  grep -i gt9 /proc/interrupts                           # 确认中断 38 已注册
} >> $LOG 2>&1                                          # 标准输出+错误都进日志
