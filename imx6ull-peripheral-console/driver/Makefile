# 内核源码目录：编译模块时要借内核的头文件和编译规则，所以路径必须指对
KERNELDIR := /home/fxh/linux/IMX6ULL/linux/linux-imx-rel_imx_4.1.15_2.1.0_ga

# 模块源码所在目录，$(shell pwd) 就是执行 make 时所在的这个文件夹
CURRENT_PATH := $(shell pwd)

# 要编译成模块（.ko）的目标，多个模块就用空格隔开
obj-m := chrdevbase.o

# 默认目标，直接敲 make 就走这里
build: kernel_modules

# 核心一句：-C 切到内核目录，M= 指定模块源码位置，modules 是内核提供的编译目标
# 这样编出来的 .ko 用的是 4.1.15 内核的头文件和 ABI，才能在这块板上加载
kernel_modules:
	$(MAKE) -C $(KERNELDIR) M=$(CURRENT_PATH) modules

# 清理编译产物（.o、.ko、中间文件）
clean:
	$(MAKE) -C $(KERNELDIR) M=$(CURRENT_PATH) clean
