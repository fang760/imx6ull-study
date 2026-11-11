#include "main.h"

/*使能I.MX6U所有外设时钟*/
void clk_enable(void)
{
    CCM_CCGR0 = 0xffffffff;
    CCM_CCGR1 = 0xffffffff;
    CCM_CCGR2 = 0xffffffff;
    CCM_CCGR3 = 0xffffffff;
    CCM_CCGR4 = 0xffffffff;
    CCM_CCGR5 = 0xffffffff;
    CCM_CCGR6 = 0xffffffff;
}

/*初始化LED对应的GPIO*/
void led_init(void)
{
    SW_MUX_GPIO1_IO03 = 0x5;    //初始化IO复用
    SW_PAD_GPIO1_IO03 = 0X10B0; //配置IO属性
    GPIO1_GDIR = 0X0000008;     //设置为输出
    GPIO1_DR=0X0;               //输出低电平，打开LED0
}

/*打开LED灯*/
void led_on(void)
{
    GPIO1_DR &= ~(1<<3);    
}

/*关闭LED灯*/
void led_off(void)
{
    GPIO1_DR |= (1<<3);    
}

/*
* @description :短时间延时函数
* @param -n :要延时循环次数，空操作循环次数
*/
void delay_short(volatile unsigned int n)
{
    while(n--){}
}

/*
* @description :延时函数，在396Mhz的主频下延时时间大约为1ms
* @param -n :要延时ms数
*/
void delay(volatile unsigned int n)
{
    while(n--)
    {
        delay_short(0x7ff);
    }
}

/*main函数*/
int main(void)
{
    clk_enable();
    led_init();
    while (1)
    {
        led_off();
        delay(500);
        led_on();
        delay(500);
    }
    return 0;
}
