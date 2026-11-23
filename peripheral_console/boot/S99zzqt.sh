#!/bin/sh
# ==============================================================================
#  /etc/rc5.d/S99zzqt  ——  开机最后一步：启动 Qt 界面
# ==============================================================================
#  为什么放在 /etc/rc5.d/：
#    inittab 里 id:5:initdefault 表示默认进运行级 5，进级时会跑 /etc/rc5.d/ 下的脚本。
#    界面必须"最后一个"起来（S99），否则前面的服务（getty 文字控制台、X 桌面等）
#    会去抢 framebuffer，把画面盖掉或刷成花屏。
#
#  三件事：
#    1) 解绑文字控制台（vtcon1），不让内核往 fb 上打字符
#    2) 告诉 Qt 用哪个显示平台、哪个触摸设备
#    3) 后台启动 /root/console（nohup + 重定向，保证关串口也不退出）
# ==============================================================================

sleep 2                                                  # 等前面服务起完，抢屏风险更小

# ① 解绑虚拟终端控制台：vtcon1 是内核的 framebuffer 文字控制台，
#    绑定时会把 printk/光标画面画到 /dev/fb0 上。写 0 解绑后 fb 只归我的程序用。
#    2>/dev/null：老内核没这个节点时不要报错打断脚本
echo 0 > /sys/class/vtconsole/vtcon1/bind 2>/dev/null

# ② Qt 平台插件环境变量
#    QT_QPA_PLATFORM=linuxfb  → 直接用 Linux Framebuffer（/dev/fb0），不依赖 X11，
#                               省内存、启动快，嵌入式标准做法
#    QT_QPA_GENERIC_PLUGINS   → 启用通用输入插件 evdevtouch，读 /dev/touch
#    （/dev/touch 是开机脚本按实际事件号建出来的软链接，指到 gt9xx 的 event 节点）
export QT_QPA_PLATFORM=linuxfb
export QT_QPA_GENERIC_PLUGINS=evdevtouch:/dev/touch
export QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS=/dev/touch

cd /root                                                 # console 程序依赖当前目录，先切过去
# ③ 后台运行界面程序
#    nohup      ：忽略挂断信号，串口断开也不退出
#    > qt_run.log 2>&1 ：输出和错误都写日志，方便排查
#    < /dev/null      ：不从终端读输入，彻底和串口脱钩
#    末尾 &           ：放后台，不阻塞开机流程
nohup ./console > /root/qt_run.log 2>&1 < /dev/null &
