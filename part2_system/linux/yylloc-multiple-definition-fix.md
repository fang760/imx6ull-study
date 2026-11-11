# 编译报错：multiple definition of `yylloc`（dtc 重定义）问题及解决方法

> 适用场景：i.MX6ULL Linux 4.1.15 老内核，Ubuntu 高版本 GCC（>=10）编译时报错

## 一、报错特征



```
/usr/bin/ld: scripts/dtc/dtc-parser.tab.o:(.bss+0x50): multiple definition of `yylloc';
scripts/dtc/dtc-lexer.lex.o:(.bss+0x0): first defined here
```



* 核心报错：**multiple definition of&#x20;**`yylloc`，即符号 `yylloc` 在两个目标文件里同时定义，链接阶段冲突；

* 阶段：这是**链接（LD）阶段错误**，不是 C 语法编译（CC）错误；前面 CC 都通过，到 LD 链接才炸；

* 位置：报错在 `scripts/dtc/`（设备树编译器 dtc），HOSTLD 编译的是跑在 Ubuntu 本机的 x86 工具，不是板子的 ARM 程序。

## 二、产生原因



1. GCC10+ 默认开启 `-fno-common`，不允许全局变量在多处定义（老 gcc 默认允许）；

2. 4.1.15 老内核自带的 dtc（flex/bison 生成代码）中，`yylloc` 变量在两处同时定义：

* `dtc-parser.tab.c`（bison 生成）

* `dtc-lexer.lex.c`（flex 生成）

1. **关键坑**：内核还带预生成文件 `dtc-lexer.lex.c_shipped`，编译时优先使用它，**只改&#x20;**`.l`**&#x20;源码文件无效，必须改&#x20;**`_shipped`**&#x20;文件**。

## 三、两个解决方法

### 方法一：永久解决（推荐）—— 改源码，把定义改成 extern 声明

原理：`yylloc` 只需要一处定义（parser 那边保留），lex 这边改成 extern 声明，就不会重复。

在内核根目录执行两条 sed 命令，然后重新编译：



```
sed -i 's/YYLTYPE yylloc;/extern YYLTYPE yylloc;/' scripts/dtc/dtc-lexer.l
sed -i 's/YYLTYPE yylloc;/extern YYLTYPE yylloc;/' scripts/dtc/dtc-lexer.lex.c_shipped
make clean
make -j16
```



* 必须 `make clean` 清掉旧 `.o`，否则旧缓存还会报错；

* 两个文件都要改：`.l` 是源码，`_shipped` 是编译实际使用的成品文件；

* 这是治本方案，以后换环境、换 gcc 都不会再犯。

### 方法一补充：extern 到底是什么（原理必读）

**定义 vs 声明 —— 一个变量只能定义一次，但可以被 extern 声明无数次**

| 写法 | 作用 | 内存 |
| --- | --- | --- |
| `YYLTYPE yylloc;` | 定义：为变量分配内存空间 | 分配 |
| `extern YYLTYPE yylloc;` | 声明：不分配内存，告诉编译器"它定义在别处，这里借用" | 不分配 |

比喻：定义 = 盖房子（全工程只能有一栋）；extern = 挂路牌"前方有房"，自己不盖房。

* 改前：`dtc-parser.tab.c` 和 `dtc-lexer.lex.c` 各定义了一份 yylloc（盖了两栋同名房）→ 链接时找到两个实体 → multiple definition 报错；
* 改后：只保留 parser 一处定义，lex 改成 extern 声明（挂路牌引用同一栋房）→ 全工程只有一个实体 → 链接通过；
* 实战规则：**一个变量在 .c 里定义一次，其他文件用 extern 声明引用**（头文件只放声明，不放定义）。

### 方法二：临时解决（备选）—— 编译命令加 `-fcommon`，不改源码

不想改源码时，在编译命令后面加一小句 `HOSTCFLAGS="-fcommon"` 即可：



```
make -j16 HOSTCFLAGS="-fcommon"
```

如果编译命令带了其他参数（ARCH / CROSS\_COMPILE），一起写上：



```
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -j16 HOSTCFLAGS="-fcommon"
```

原理：GCC10+ 默认开启 `-fno-common`（同名全局变量只能定义一次），dtc 里 `yylloc` 两处定义就报错；加 `-fcommon` 让编译器把同名全局变量合并成一个 common 符号，回到 GCC10 之前的宽松行为，直接绕过报错。`HOSTCFLAGS` 是编译主机工具（dtc 属于 host 工具）的编译参数。



* 优点：不改任何源码文件，能快速跑通；

* 缺点：只是规避、不是治本；**每次编译都要带上这句**（或写进编译脚本固定住），换台机器 / 换环境又会犯；

* 适合：临时跑通编译验证；**长期仍建议用方法一改源码**。

## 四、排查步骤（通用流程）



1. 看报错日志：找到两个输出 `yylloc` 的 `.o` 文件，定位对应源码路径；

2. 搜索定位所有出现位置：



```
grep -rn "YYLTYPE yylloc" scripts/dtc/
```



1. 判断哪个是源码（`.l`），哪个是编译实际用的预生成文件（`.c_shipped`）；

2. 把多余的 "定义" 改成 "extern 声明"（extern = 只声明变量存在、不开辟空间；不带 extern = 定义、开辟空间）；

3. 清理编译产物再编译：`make clean` 后重新 `make`。

## 五、记忆点（哪些要记）

🔥 必须记住：



1. 报错 `multiple definition of yylloc` = **老内核 dtc + 高版本 GCC（>=10）** 的经典组合坑；

2. 两个文件都要改：`dtc-lexer.l` 和 `dtc-lexer.lex.c_shipped`，改完一定 `make clean`；

3. HOSTLD = 编译 Ubuntu 本机工具（x86），不是 ARM 程序，看到它别慌。

📖 理解即可：



* `-fno-common`：GCC10 新规则，全局变量不能多处定义；

* `yylloc`：flex/bison 语法分析器里记录位置行号的变量。

🚫 不用记：



* 具体行号、地址，每次编译会变，用 grep 搜索字符串定位即可。

## 六、扩展：所有 multiple definition 报错的通用套路

> 不只是 yylloc，任何
> `multiple definition of xxx`
> 都按这个思路排查：



1. 头文件里写了完整函数 / 变量定义，被多个 `.c` include → 最高发原因；

2. 两个不同 `.c` 写了同名全局函数；

3. 规则：**.h 只放声明（extern），.c 放实现（定义）**。