.global _start  @全局标号

_start:

/*进入SVC模式 */
mrs r0,cpsr
bic r0,r0,#0x1f
orr r0,r0,#0x13
msr cpsr,r0

/*设置栈指针 */
ldr sp,=0X80200000

b main @跳转到mian
