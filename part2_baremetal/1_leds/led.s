.global _start 

_start:

@1.使能所有时钟CCGR0-CCGR6
ldr r0,=0x020C4068
ldr r1,=0xFFFFFFFF
str r1,[r0]

ldr r0,=0x020C406C
str r1,[r0]

ldr r0,=0x020C4070
str r1,[r0]

ldr r0,=0x020C4074
str r1,[r0]

ldr r0,=0x020C4078
str r1,[r0]

ldr r0,=0x020C407C
str r1,[r0]

ldr r0,=0x020C4080
str r1,[r0]


@2.设置GPIO1——IO03复用为GPIO1_IO03
ldr r0,=0x020E0068
ldr r1,=0x5
str r1,[r0]

@3.配置GPIO1_IO03的IO属性
ldr r0,=0x020E02F4
ldr r1,=0x10B0
str r1,[r0]

@4.设置GPIO1_IO03为输出
ldr r0,=0x0209C004
ldr r1,=0x0000008
str r1,[r0]

@5.打开LED0，设置GPIO1_IO03输出低电平
ldr r0,=0x0209C000
ldr r1,=0
str r1,[r0]

@6.死循环
loop:
    b loop


