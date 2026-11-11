#include "imx6ul.h"

//使能I.MX6U所有外设时钟
void clk_enable(void)
{
    CCM->CCGR0 = 0XFFFFFFFF;
    CCM->CCGR1 = 0XFFFFFFFF;
    CCM->CCGR2 = 0XFFFFFFFF;
    CCM->CCGR3 = 0XFFFFFFFF;
    CCM->CCGR4 = 0XFFFFFFFF;
    CCM->CCGR5 = 0XFFFFFFFF;
    CCM->CCGR6 = 0XFFFFFFFF;
}

//初始化LED对应的GPIO
void led_init(void)
{
    IOMUX_SW_MUX->GPIO1_IO03 = 0X05;//初始化IO复用
    IOMUX_SW_PAD->GPIO1_IO03 = 0X10B0;//配置IO属性
    GPIO1->GDIR = 0X00000008;//GPIO方向设置为输出
    GPIO1->DR &= ~(1<<3);//设置输出低电平，打开LED0
}

//打开LED灯
void led_on(void)
{
    GPIO1->DR &= ~(1<<3);
}
//关闭LED灯
void led_off(void)
{
    GPIO1->DR |= 1<<3;
}

//短时间延时函数,空操作循环次数
void delay_short(volatile unsigned int n)
{
    while(n--){}
}

//延时函数(ms)
void delay(volatile unsigned int n)
{
    while(n--)
    {
        delay_short(0x7ff);
    }
}

//main函数
int main(void)
{
    clk_enable();
    led_init();
    while(1)
    {
        led_off();
        delay(500);
        led_on();
        delay(500);
    }
    return 0;
}