# ==============================================================================
#  console.pro  ——  Qt 工程文件（qmake 读它来生成 Makefile）
# ==============================================================================
#  用法（Ubuntu 上）：
#    . /opt/fsl-imx-x11/4.1.15-2.1.0/environment-setup-cortexa7hf-neon-poky-linux-gnueabi
#    qmake            # 读本文件生成 Makefile
#    make -j4         # 交叉编译出 ARM 版 console
#
#  说明：qmake 是"读 .pro → 生成 Makefile → make 编译"的两段式，
#        所以改了这个文件之后必须重新执行 qmake，直接 make 不会生效。
# ==============================================================================

# QT 变量：声明要用 Qt 的哪些模块
#   core    → QtCore（QString、QFile、QTimer、QDateTime 等非界面类）
#   gui     → QtGui（QFont、QPainter、事件系统）
#   widgets → QtWidgets（QWidget、QPushButton、QSlider、QGridLayout 等）
# 只写需要的模块能让链接体积更小，嵌入式上很重要
QT += core gui widgets

# CONFIG：编译选项
#   c++11 → 允许使用 C++11 语法（本项目用了 lambda、override、nullptr）
CONFIG += c++11

# TARGET：最终可执行文件名，编译出来就叫 console（板上 /root/console 就是它）
TARGET = console

# SOURCES：参与编译的源文件列表（本项目界面只有一个文件）
SOURCES += main.cpp
