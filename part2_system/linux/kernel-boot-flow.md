# Linux 内核启动流程分析

> 对应实践：从链接脚本入手，重点分析汇编入口 stext、C 语言入口 start_kernel、rest_init 创建的三个进程，以及 init 进程如何挂载根文件系统、进入用户态。
> 核心链路：**stext → start_kernel → rest_init → kernel_init → 用户空间**。

---

## 一、内核启动前的约定

U-Boot 执行 `bootz` 跳转内核前，必须满足以下条件，否则内核无法启动：

| 状态/寄存器 | 值 | 说明 |
|---|---|---|
| MMU | 关闭 | 内存管理单元（Memory Management Unit） |
| D-cache | 关闭 | 数据缓存（Data Cache） |
| r0 | 0 | 固定传 0 |
| r1 | machine nr | 机器 ID（Machine Number） |
| r2 | dtb 首地址 | 设备树在内存中的起始地址 |

> r0、r1、r2 由 U-Boot 负责设置，内核启动后从 r1、r2 取出机器 ID 和设备树地址。I-Cache 开不开无所谓，不用记。

---

## 二、内核入口 stext（重点）

内核链接脚本 `arch/arm/kernel/vmlinux.lds` 用 `ENTRY(stext)` 指定入口，stext 定义在 `arch/arm/kernel/head.S`，是内核启动的第一段汇编代码。

### 1. 主要代码

```asm
ENTRY(stext)
    safe_svcmode_maskall r9        @ 进入 SVC 模式，关闭所有中断
    mrc p15, 0, r9, c0, c0        @ 读取处理器 ID
    bl  __lookup_processor_type    @ 检查内核是否支持当前 CPU
    bl  __vet_atags                @ 校验设备树合法性
    bl  __create_page_tables       @ 创建一级页表
    ldr r13, =__mmap_switched     @ 记录开 MMU 后跳转地址
    bl  __enable_mmu               @ 使能 MMU
ENDPROC(stext)
```

### 2. 核心任务：开启 MMU

- **进入 SVC 模式、关中断**：SVC（Supervisor，管理模式）是 ARM 特权模式，内核需要最高权限才能访问所有硬件；同时关闭中断，保证启动过程不被打断。
- **检查 CPU、校验设备树**：确认内核支持当前 CPU，且 r2 指向的设备树合法，否则启动失败。
- **建立页表**：页表（Page Table）是 MMU 实现"虚拟地址 → 物理地址"转换的映射表，开 MMU 前必须先建好。
- **开启 MMU**：使能后 CPU 开始使用虚拟地址，完成后跳到 `__mmap_switched`。

> 小结：stext 的核心使命就是"**搭好页表、开启 MMU，然后交出控制权**"。具体汇编指令和页表建立过程不用记。

---

## 三、__mmap_switched（不用深究）

MMU 开启后经过 `__mmap_switched`（在 `head-common.S`），设置好栈、清掉 `.bss` 段，最后跳转到 **start_kernel**，进入 C 语言阶段。这是汇编到 C 的过渡，不用深究。

---

## 四、start_kernel（重点）

`start_kernel` 定义在 `init/main.c`，是内核的 **C 语言入口**，通过调用大量子函数把内存、调度、中断等各个子系统初始化起来。

子函数很多，只需要重点掌握两个：

| 函数 | 作用 |
|---|---|
| `setup_arch` | 架构初始化，**解析设备树、获取 bootargs** |
| `console_init` | 控制台初始化，**之后 printk 才输出到串口** |

（其余如 mm_init、sched_init、init_IRQ 等负责初始化内存、调度、中断，知道有这回事即可，不用记名字。）

### 重点理解：

**① setup_arch —— 解析设备树**
- 解析 U-Boot 用 r2 传进来的设备树，根据 `model`、`compatible` 确认内核是否支持该单板；
- 从设备树 `chosen` 节点取出 bootargs（启动参数，如 console、root 等）。

**② 为什么串口一开始没输出**
- console_init 之前的 printk 都存在缓冲区里，不会显示；调用 console_init 后才真正从串口打印；
- 用哪个串口由 bootargs 里的 `console=ttymxc0,115200` 指定。

**③ BogoMIPS（顺带了解）**
- 全称 Bogus MIPS（"假的 MIPS"），是内核启动时粗略测出的 CPU 速度指标，用来校准延时，不是真实跑分。

start_kernel 最后调用 `rest_init`，进入进程创建阶段。

---

## 五、rest_init 与三个进程（重点）

`rest_init` 定义在 `init/main.c`，是 start_kernel 的最后一步。它创建两个内核进程，自己则变成 idle 进程。

### 1. 三个进程

| 进程 | PID | 作用 |
|---|---|---|
| **idle** | 0 | 空闲进程，CPU 没任务时运行 |
| **init** | 1 | 第一个用户进程，挂载根fs、运行用户 init 程序 |
| **kthreadd** | 2 | 内核线程守护进程，管理所有内核线程 |

### 2. 逐个理解

**① idle 进程（PID=0）**
- 空闲进程，类似 FreeRTOS/RT-Thread 的空闲任务；CPU 没有其他进程运行时就执行 idle，一旦有进程就绪会立刻被抢占。

**② init 进程（PID=1）**
- 系统中**所有用户进程的祖先**；
- 一开始运行在内核态，后面挂载根文件系统、运行用户空间 init 程序，完成"内核态 → 用户态"的转变，是启动流程中最关键的进程。

**③ kthreadd 进程（PID=2）**
- 内核线程的"管家"，系统中所有内核线程都由它创建和管理。

> 记忆口诀：**0 号空闲、1 号 init、2 号 kthreadd**。

---

## 六、init 进程如何进入用户态（重点）

init 进程的函数是 `kernel_init`，定义在 `init/main.c`，最终完成从内核到用户空间的跨越。

### 1. 关键动作

```
① 初始化设备驱动
② 打开 /dev/console，建立标准输入/输出/错误（0/1/2）
③ 挂载根文件系统（prepare_namespace）
④ 运行用户空间 init 程序
```

### 2. 逐项说明

**① 初始化设备驱动**：编译进内核的各类驱动在这一阶段被加载、初始化，硬件才能工作。

**② 建立标准输入/输出/错误**：打开 `/dev/console` 得到文件描述符 0（标准输入），再复制两次得到 1（标准输出）、2（标准错误），三者都指向控制台，所以启动后能在串口交互。

**③ 挂载根文件系统（关键）**：
- 根文件系统（rootfs）是内核挂载的第一个文件系统，用户程序都存放在里面；
- 位置由 bootargs 里的 `root=xxx` 指定，例如 `root=/dev/mmcblk1p2 rootwait rw`；
- **没有根文件系统，内核就找不到用户程序，会报 `VFS: Unable to mount root fs`，无法进入用户态。**

**④ 运行用户 init 程序**：
- 内核查找用户空间 init 程序并运行（可用 bootargs 的 `init=xxx` 指定，否则按默认路径查找）；
- 运行成功后正式进入用户态，最终出现登录界面；全部找不到则 `panic`。

> 这一步真正实现了**内核态 → 用户态**的转变，也是内核必须配合根文件系统才能启动到界面的原因。init 程序查找的具体备用路径不用记。

---

## 七、完整启动流程

```
U-Boot：bootz（关 MMU/D-cache，r0=0、r1=机器ID、r2=dtb）
   │
   ▼
stext（head.S，汇编）
   进 SVC 模式、检查 CPU/设备树、建页表、开启 MMU
   │
   ▼
__mmap_switched（过渡，设栈、清 bss）
   │
   ▼
start_kernel（main.c，C 语言）
   setup_arch 解析设备树、console_init 串口开始输出
   │
   ▼
rest_init —— 产生三个进程
   init（PID=1）、kthreadd（PID=2）、idle（PID=0）
   │
   ▼
kernel_init（init 进程）
   初始化驱动、打开 /dev/console（0/1/2）
   挂载根文件系统、运行用户 init 程序
   │
   ▼
用户空间（登录界面）
```

---

## 八、本章小结

这一章主要搞清楚一件事：**Linux 内核是怎么从一段汇编代码一步步启动到用户登录界面的。**

1. **内核入口**：链接脚本用 `ENTRY(stext)` 指定 stext 为入口。
2. **汇编阶段**：stext 进入 SVC 模式、建页表、开启 MMU，完成底层硬件准备。
3. **C 语言阶段**：start_kernel 初始化各个子系统，setup_arch 解析设备树，console_init 后串口才输出。
4. **三个进程**：rest_init 产生 idle(0)、init(1)、kthreadd(2)，init 是所有用户进程的祖先。
5. **进入用户态**：init 初始化驱动、打开控制台、挂载根文件系统、运行用户 init 程序。

**一句话概括**：U-Boot 引导 → stext 开 MMU → start_kernel 初始化 → rest_init 建进程 → init 挂载根文件系统 → 用户空间。

> 这也解释了为什么一个可启动的 Linux 系统必须同时具备 **U-Boot、内核镜像、根文件系统** 三部分——它们分别负责引导、系统核心和用户程序。

---

## 附：源码位置速查

| 内容 | 路径 |
|---|---|
| 链接脚本 | `arch/arm/kernel/vmlinux.lds` |
| 汇编入口 | `arch/arm/kernel/head.S` |
| C 入口及各进程 | `init/main.c` |

