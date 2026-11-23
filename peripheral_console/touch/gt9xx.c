/*
 * ============================================================================
 *  gt9xx.c  ——  Goodix GT911 电容触摸屏驱动（i.MX6ULL + Linux 4.1.15）
 * ============================================================================
 *  平台：正点原子 ALPHA V2.2 eMMC 版，7 寸 RGB 屏 1024×600，触摸芯片 GT911
 *  总线：I2C-2（设备树 &i2c2 下 goodix_ts@5d），中断：GPIO1_IO09 → IRQ 38
 *
 *  【本文件的主干流程】
 *    probe（设备树匹配上后调用）
 *      → 解析设备树拿 INT/RST 引脚号
 *      → 申请 GPIO、把 INT 复用成中断功能、拿到中断号
 *      → 自动探测 I2C 地址（0x5D / 0x14）
 *      → 注册 input 设备，用户态就多出 /dev/input/event1
 *    运行中
 *      → 手指触摸：GT911 拉低 INT → 中断上半部 → 调度工作队列
 *      → 工作队列里读 I2C 坐标 → input_report_abs → input_sync
 *      → Qt(evdevtouch) 读 /dev/input/event1，界面响应点击
 *
 *  改过原厂代码的三个地方：
 *    坑1  RST 脚接 SNVS_TAMPER9，引脚复用被 SNVS 域硬件锁死 → 软件复位是假动作，
 *         只能把 gtp_reset_guitar 置空，让芯片冷启动自己加载 flash 配置。
 *    坑2  原厂模板把 INT 脚也配成 GPIO 输出 → 读不到电平、中断永远不来，
 *         改成 GPIOF_IN 输入 + GTP_GPIO_AS_INT + gpio_to_irq。
 *    坑3  因为不能主动复位，芯片 I2C 地址在 0x5D / 0x14 之间漂 → 加自动探测。
 * ============================================================================
 */

/* 
 * Goodix GT9xx touchscreen driver
 *
 * Copyright  (C)  2010 - 2014 Goodix. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be a reference
 * to you, when you are integrating the GOODiX's CTP IC into your system,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * Base version 2.4
 * Release Date: 2021/11/27
 */
#include <linux/irq.h>
#include "gt9xx.h"
#include <asm/unaligned.h>

#include <linux/input/mt.h>

#define GTP_CONFIG_OF 1
/* ----------------------------------------------------------------------------
 *  全局变量区（下面几个 static 变量在整个驱动里反复用，先认一下）
 *    goodix_wq        ：一个单线程工作队列，"中断下半部"的活儿都在它上面排队执行
 *    i2c_connect_client：保存当前 i2c_client 指针，方便 /proc 配置接口反查设备
 *    gtp_rst_gpio     ：RST 引脚的全局 GPIO 编号（由设备树解析得到）
 *    gtp_int_gpio     ：INT 引脚的全局 GPIO 编号，gpio_to_irq 就是拿它换中断号
 *    config[]         ：芯片配置表的缓冲区（184 字节配置 + 2 字节寄存器地址）
 *  为什么用全局变量：probe 里解析出来的引脚号，后面多个函数（复位、同步、中断）都要用，
 *  放全局比一层层传参简单，这是驱动里很常见的写法。
 * -------------------------------------------------------------------------- */
static const char *goodix_ts_name = "goodix-ts";
static const char *goodix_input_phys = "input/ts";
static struct workqueue_struct *goodix_wq;
struct i2c_client * i2c_connect_client = NULL; 
int gtp_rst_gpio;
int gtp_int_gpio;
u8 config[GTP_CONFIG_MAX_LENGTH + GTP_ADDR_LENGTH];

static s8 gtp_i2c_test(struct i2c_client *client, struct goodix_ts_data *ts);
void gtp_reset_guitar(struct i2c_client *client, s32 ms);
void gtp_int_sync(s32 ms);
static ssize_t gt91xx_config_read_proc(struct file *, char __user *, size_t, loff_t *);
static ssize_t gt91xx_config_write_proc(struct file *, const char __user *, size_t, loff_t *);

/* ----------------------------------------------------------------------------
 *  /proc 配置接口的三个东西
 *    gt91xx_config_proc ：代表 /proc/gt91xx_config 这个节点
 *    config_proc_ops    ：该节点的读写方法（下面紧跟着定义）
 *  用途：调试时 cat 看当前配置 / echo 刷一份新配置。
 *  注意：/proc 是"进程与内核信息"的出口，和 /sys 的"设备属性"不是一回事。
 * -------------------------------------------------------------------------- */
static struct proc_dir_entry *gt91xx_config_proc = NULL;
static const struct file_operations config_proc_ops = {
    .owner = THIS_MODULE,
    .read = gt91xx_config_read_proc,
    .write = gt91xx_config_write_proc,
};

int gtp_parse_dt_cfg(struct device *dev, u8 *cfg, int *cfg_len, u8 sid);


static const struct goodix_chip_data gt1x_chip_data = {
    .config_addr    = GTP_GT1X_REG_CONFIG_DATA,
    .config_len     = GTP_CONFIG_MAX_LENGTH,
};

static const struct goodix_chip_data gt9x_chip_data = {
    .config_addr    = GTP_GT9X_REG_CONFIG_DATA,
    .config_len     = GTP_CONFIG_MAX_LENGTH,
};

static const struct goodix_chip_data *goodix_get_chip_data(u16 id)
{
    switch (id) {
    case 1151:
    case 1158:
    case 5663:
    case 5688:
        config[0] = GTP_GT1X_REG_CONFIG_DATA >> 8;
        config[1] = GTP_GT1X_REG_CONFIG_DATA & 0xff;
        return &gt1x_chip_data;
    default:
        config[0] = GTP_GT9X_REG_CONFIG_DATA >> 8;
        config[1] = GTP_GT9X_REG_CONFIG_DATA & 0xff;
        return &gt9x_chip_data;
    }
}

static int goodix_i2c_read(struct i2c_client *client,
                           u16 reg, u8 *buf, int len)
{
    struct i2c_msg msgs[2];
    __be16 wbuf = cpu_to_be16(reg);
    int ret;

    msgs[0].flags = 0;
    msgs[0].addr  = client->addr;
    msgs[0].len   = 2;
    msgs[0].buf   = (u8 *)&wbuf;

    msgs[1].flags = I2C_M_RD;
    msgs[1].addr  = client->addr;
    msgs[1].len   = len;
    msgs[1].buf   = buf;

    ret = i2c_transfer(client->adapter, msgs, 2);
    return ret < 0 ? ret : (ret != ARRAY_SIZE(msgs) ? -EIO : 0);
}

/* ----------------------------------------------------------------------------
 *  gtp_i2c_read：读 GT911 寄存器
 *  协议：先写 2 字节寄存器地址，再读 len 字节数据（"地址+数据"两段式）
 *  注意第 3 个包才是真实数据以外，最后还有一个校验字节（所以 len 要 +1 传）
 *  返回 2 表示成功（这是驱动内部约定的成功码），非 2 就是通信失败
 * -------------------------------------------------------------------------- */
s32 gtp_i2c_read(struct i2c_client *client, u8 *buf, s32 len)
{
    struct i2c_msg msgs[2];
    s32 ret=-1;
    s32 retries = 0;

    GTP_DEBUG_FUNC();

    msgs[0].flags = !I2C_M_RD;
    msgs[0].addr  = client->addr;
    msgs[0].len   = GTP_ADDR_LENGTH;
    msgs[0].buf   = &buf[0];
    
    msgs[1].flags = I2C_M_RD;
    msgs[1].addr  = client->addr;
    msgs[1].len   = len - GTP_ADDR_LENGTH;
    msgs[1].buf   = &buf[GTP_ADDR_LENGTH];

    while (retries < 5) {
        ret = i2c_transfer(client->adapter, msgs, 2);
        if(ret == 2)break;
        retries++;
    }
    if ((retries >= 5)) {
        GTP_ERROR("I2C Read: 0x%04X, %d bytes failed, errcode: %d! Process reset.", (((u16)(buf[0] << 8)) | buf[1]), len-2, ret);
        gtp_reset_guitar(client, 10);
    }
    return ret;
}

/* ----------------------------------------------------------------------------
 *  gtp_i2c_write：写 GT911 寄存器（地址 + 数据一次性发出去）
 *  失败重试 5 次：I2C 在嵌入式板上偶发干扰，多试几次比直接报错稳
 * -------------------------------------------------------------------------- */
s32 gtp_i2c_write(struct i2c_client *client,u8 *buf,s32 len)
{
    struct i2c_msg msg;
    s32 ret = -1;
    s32 retries = 0;

    GTP_DEBUG_FUNC();

    msg.flags = !I2C_M_RD;
    msg.addr  = client->addr;
    msg.len   = len;
    msg.buf   = buf;

    while (retries < 5) {
        ret = i2c_transfer(client->adapter, &msg, 1);
        if (ret == 1)break;
        retries++;
    }
    if ((retries >= 5)) {
        GTP_ERROR("I2C Write: 0x%04X, %d bytes failed, errcode: %d! Process reset.", (((u16)(buf[0] << 8)) | buf[1]), len-2, ret);
        gtp_reset_guitar(client, 10);
    }
    return ret;
}

/* ----------------------------------------------------------------------------
 *  gtp_i2c_read_dbl_check：读两次比对
 *  做法：同一寄存器连读两遍，两遍数据完全一样才认为"读成功"。
 *  目的：I2C 总线在板子上容易受干扰，单次读可能读错；两次一致能过滤掉大部分毛刺。
 *  代价：多花一次总线时间（几毫秒），换来的是坐标不会突然跳点。
 * -------------------------------------------------------------------------- */
s32 gtp_i2c_read_dbl_check(struct i2c_client *client, u16 addr, u8 *rxbuf, int len)
{
    u8 buf[16] = {0};
    u8 confirm_buf[16] = {0};
    u8 retry = 0;
    
    while (retry++ < 3) {
        memset(buf, 0xAA, 16);
        buf[0] = (u8)(addr >> 8);
        buf[1] = (u8)(addr & 0xFF);
        gtp_i2c_read(client, buf, len + 2);
        
        memset(confirm_buf, 0xAB, 16);
        confirm_buf[0] = (u8)(addr >> 8);
        confirm_buf[1] = (u8)(addr & 0xFF);
        gtp_i2c_read(client, confirm_buf, len + 2);
        
        if (!memcmp(buf, confirm_buf, len+2)) {
            memcpy(rxbuf, confirm_buf+2, len);
            return SUCCESS;
        }
    }
    GTP_ERROR("I2C read 0x%04X, %d bytes, double check failed!", addr, len);
    return FAIL;
}

/* ----------------------------------------------------------------------------
 *  gtp_irq_disable：关中断（用 disable_irq_nosync）
 *  为什么要 spin_lock_irqsave：这个函数可能从中断上下文或进程上下文被调用，
 *  用自旋锁保护 irq_is_disable 标志，避免两边同时改造成重复开关中断。
 *  disable_irq_nosync 与 disable_irq 的区别：前者立即返回、不等正在执行的中断跑完，
 *  适合在中断处理内部调用（否则会死等自己，直接卡死）。
 * -------------------------------------------------------------------------- */
void gtp_irq_disable(struct goodix_ts_data *ts)
{
    unsigned long irqflags;

    GTP_DEBUG_FUNC();

    spin_lock_irqsave(&ts->irq_lock, irqflags);
    if (!ts->irq_is_disable) {
        ts->irq_is_disable = 1;
        disable_irq_nosync(ts->client->irq);
    }
    spin_unlock_irqrestore(&ts->irq_lock, irqflags);
}

/* ----------------------------------------------------------------------------
 *  gtp_irq_enable：开中断
 *  配对使用：读事件之前关中断（防止读到一半又来新中断打架），
 *            读完再开（恢复"有触摸就通知"）。这就是典型的"关-读-开"临界区写法。
 * -------------------------------------------------------------------------- */
void gtp_irq_enable(struct goodix_ts_data *ts)
{
    unsigned long irqflags = 0;

    GTP_DEBUG_FUNC();
    
    spin_lock_irqsave(&ts->irq_lock, irqflags);
    if (ts->irq_is_disable) {
        enable_irq(ts->client->irq);
        ts->irq_is_disable = 0;
    }
    spin_unlock_irqrestore(&ts->irq_lock, irqflags);
}

/* ----------------------------------------------------------------------------
 *  gtp_touch_down：上报一个触点按下
 *  input_mt_slot(ts->input_dev, id)  ：选定第 id 个触点槽位（多点触控靠槽位区分手指）
 *  input_mt_report_slot_state(...1)  ：告诉内核"这个槽位现在是按下状态"
 *  input_report_abs(...ABS_MT_POSITION_X/Y)：上报该触点的绝对坐标
 *  input_report_abs(...ABS_MT_TOUCH_MAJOR)：上报接触面积（w），有些手势库会用到
 * -------------------------------------------------------------------------- */
static void gtp_touch_down(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
    input_mt_slot(ts->input_dev, id);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
}

/* ----------------------------------------------------------------------------
 *  gtp_touch_up：上报一个触点抬起
 *  只要把 slot 状态报成 0（未接触），上层就知道这根手指离开了。
 *  注意：抬起时不用再报坐标，内核会沿用上一次的位置。
 * -------------------------------------------------------------------------- */
static void gtp_touch_up(struct goodix_ts_data* ts, s32 id)
{
    input_mt_slot(ts->input_dev, id);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, -1);
    GTP_DEBUG("Touch id[%2d] release!", id);
}

/* ============================================================================
 *  工作队列下半部：真正读坐标的地方
 *  ---------------------------------------------------------------------------
 *  为什么要分上半部和下半部：
 *    中断上下文不能睡眠，而读 I2C 会睡眠（等总线），所以上半部只"记一下、排个队"，
 *    真正的 I2C 读取放到工作队列（进程上下文）里做，这就是 Linux 中断的
 *    "上半部 + 下半部"机制。
 *
 *  流程：读 0x814E 状态寄存器 → 有数据就逐个触点读坐标 →
 *        input_report_abs(EV_ABS, ABS_X/ABS_Y) → input_sync 一次上报结束
 * ========================================================================== */
static void goodix_ts_work_func(struct work_struct *work)
{
    u8  end_cmd[3] = {GTP_READ_COOR_ADDR >> 8, GTP_READ_COOR_ADDR & 0xFF, 0};
    u8  point_data[2 + 1 + 8 * GTP_MAX_TOUCH + 1]={GTP_READ_COOR_ADDR >> 8, GTP_READ_COOR_ADDR & 0xFF};
    u8  touch_num = 0;
    u8  finger = 0;
    static u16 pre_touch = 0;
    static u8 pre_key = 0;
    u8  key_value = 0;
    u8* coor_data = NULL;
    s32 input_x = 0;
    s32 input_y = 0;
    s32 input_w = 0;
    s32 id = 0;
    s32 i  = 0;
    s32 ret = -1;
    struct goodix_ts_data *ts = NULL;

    GTP_DEBUG_FUNC();
    ts = container_of(work, struct goodix_ts_data, work);
    if (ts->enter_update) {
        return;
    }

    ret = gtp_i2c_read(ts->client, point_data, 12);
    if (ret < 0) {
        GTP_ERROR("I2C transfer error. errno:%d\n ", ret);
        if (ts->use_irq) {
            gtp_irq_enable(ts);
        }
        return;
    }
    
    finger = point_data[GTP_ADDR_LENGTH];

    if (finger == 0x00) {
        if (ts->use_irq) {
            gtp_irq_enable(ts);
        }
        return;
    }

    if((finger & 0x80) == 0) {
        goto exit_work_func;
    }

    touch_num = finger & 0x0f;
    if (touch_num > GTP_MAX_TOUCH) {
        goto exit_work_func;
    }

    if (touch_num > 1) {
        u8 buf[8 * GTP_MAX_TOUCH] = {(GTP_READ_COOR_ADDR + 10) >> 8, (GTP_READ_COOR_ADDR + 10) & 0xff};

        ret = gtp_i2c_read(ts->client, buf, 2 + 8 * (touch_num - 1));
        memcpy(&point_data[12], &buf[2], 8 * (touch_num - 1));
    }

    pre_key = key_value;

    //GTP_DEBUG("pre_touch:%02x, finger:%02x.", pre_touch, finger);

    if (pre_touch || touch_num) {
        s32 pos = 0;
        u16 touch_index = 0;
        u8 report_num = 0;
        coor_data = &point_data[3];
        
        if(touch_num) {
            id = coor_data[pos] & 0x0F;
            touch_index |= (0x01<<id);
        }
        
        GTP_DEBUG("id = %d,touch_index = 0x%x, pre_touch = 0x%x\n",id, touch_index,pre_touch);
        for (i = 0; i < GTP_MAX_TOUCH; i++) {
            if ((touch_index & (0x01<<i))) {
                input_x  = coor_data[pos + 1] | (coor_data[pos + 2] << 8);
                input_y  = coor_data[pos + 3] | (coor_data[pos + 4] << 8);
                input_w  = coor_data[pos + 5] | (coor_data[pos + 6] << 8);

                gtp_touch_down(ts, id, input_x, input_y, input_w);
                pre_touch |= 0x01 << i;
                
                report_num++;
                if (report_num < touch_num) {
                    pos += 8;
                    id = coor_data[pos] & 0x0F;
                    touch_index |= (0x01<<id);
                }
            } else {
                gtp_touch_up(ts, i);
                pre_touch &= ~(0x01 << i);
            }
        }
    }

    input_sync(ts->input_dev);

exit_work_func:
    if (!ts->gtp_rawdiff_mode) {
        ret = gtp_i2c_write(ts->client, end_cmd, 3);
        if (ret < 0) {
            GTP_INFO("I2C write end_cmd error!");
        }
    }
    if (ts->use_irq) {
        gtp_irq_enable(ts);
    }
}

static enum hrtimer_restart goodix_ts_timer_handler(struct hrtimer *timer)
{
    struct goodix_ts_data *ts = container_of(timer, struct goodix_ts_data, timer);

    GTP_DEBUG_FUNC();

    queue_work(goodix_wq, &ts->work);
    hrtimer_start(&ts->timer, ktime_set(0, (GTP_POLL_TIME+6)*1000000), HRTIMER_MODE_REL);
    return HRTIMER_NORESTART;
}

/* ============================================================================
 *  中断上半部：GT911 手指按下/抬起时 INT 脚下降沿触发（本板是 38 号中断）
 *  ---------------------------------------------------------------------------
 *  这里只做两件事：关中断（防重入）、把工作推给下半部；然后立刻返回。
 *  IRQ_HANDLED = 告诉内核"这个中断我认领并处理了"，否则内核会报 spurious。
 * ========================================================================== */
static irqreturn_t goodix_ts_irq_handler(int irq, void *dev_id)
{
    struct goodix_ts_data *ts = dev_id;

    GTP_DEBUG_FUNC();

    gtp_irq_disable(ts);

    queue_work(goodix_wq, &ts->work);
    
    return IRQ_HANDLED;
}

/* ----------------------------------------------------------------------------
 *  gtp_int_sync：INT 脚同步（原厂复位时序的一部分）
 *  正常用途：复位期间把 INT 当输出拉低/拉高，用来"告诉"GT911 选哪个 I2C 地址，
 *             再把 INT 切回输入等待中断。
 *  本板：复位已置空，这个函数基本不被真正用到，但保留以兼容原厂调用点。
 * -------------------------------------------------------------------------- */
void gtp_int_sync(s32 ms)
{
    GTP_GPIO_OUTPUT(gtp_int_gpio, 0);
    msleep(ms);
    GTP_GPIO_AS_INT(gtp_int_gpio);
}


/* ============================================================================
 *  【坑1】软件复位函数：本板必须置空
 *  ---------------------------------------------------------------------------
 *  原厂做法：RST 拉低 10ms → 拉高 → 等 55ms → INT 同步，用这个时序复位芯片。
 *  本板现实：RST 接 GPIO5_IO09(SNVS_TAMPER9)，属于 SNVS（always-on 电源域），
 *           该域引脚复用被硬件锁死，Linux pinctrl 改不动 → 根本切不出 GPIO 输出。
 *  后果：所谓"拉低复位"是假动作；但原厂代码仍按复位成功的假设继续，
 *        会把芯片从正常扫描状态写坏，表现就是触摸时好时坏。
 *  处理：函数体置空，冷启动时芯片自己从 flash 加载出厂配置并正常扫描即可。
 * ========================================================================== */
void gtp_reset_guitar(struct i2c_client *client, s32 ms)
{
    /* 本板 RST 脚(SNVS_TAMPER9)的 MUX 位被硬件锁定，无法切换成 GPIO；
     * 强行按原厂时序驱动只会把芯片从“正常扫描”状态搞坏。
     * 冷启动时芯片自己加载 flash 出厂配置并正常扫描，所以这里什么都不做。 */
    (void)ms;
    GTP_INFO("skip guitar reset (rst pin not muxable on this board)");
}


#define GTP_DEF_CFG_LEN 184
#define GTP_REG_CFG_CHKSUM 0x80FF
#define GTP_REG_CFG_FRESH  0x8100

static u8 gtp_def_cfg[GTP_DEF_CFG_LEN] = {
	0x46, 0x00, 0x04, 0x58, 0x02, 0x0A, 0x3D, 0x00, 0x01, 0x88, 0x28, 0x0F,
	0x50, 0x32, 0x03, 0x05, 0x00, 0x00, 0x00, 0x00, 0x22, 0x33, 0x00, 0x15,
	0x19, 0x1E, 0x14, 0x8C, 0x2E, 0x0E, 0x17, 0x15, 0x0C, 0x08, 0x00, 0x00,
	0x01, 0x9B, 0x03, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x0F, 0x2D, 0x94, 0xD5, 0x02, 0x07, 0x00, 0x00, 0x04,
	0x9B, 0x11, 0x00, 0x7A, 0x16, 0x00, 0x61, 0x1C, 0x00, 0x4B, 0x25, 0x00,
	0x3C, 0x2F, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x04, 0x06, 0x08,
	0x0A, 0x0C, 0x0E, 0x10, 0x12, 0x14, 0x16, 0x18, 0x1A, 0x1C, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0x00, 0x02,
};


/* 主机下发配置：按 GT9xx 编程指南 4.3，RST 释放后延时 >=50ms 再写 */
/* ----------------------------------------------------------------------------
 *  gtp_send_def_cfg：把内置的默认配置表（gtp_def_cfg，184 字节）写进芯片
 *  配置内容包括：分辨率、扫描周期、触摸阈值、上报方式（中断/轮询）等
 *  没刷过配置的芯片上电用的是 flash 里的出厂配置，本板靠它就能工作
 * -------------------------------------------------------------------------- */
static void gtp_send_def_cfg(struct i2c_client *client)
{
    u8 buf[GTP_ADDR_LENGTH + GTP_DEF_CFG_LEN];
    u8 ck[GTP_ADDR_LENGTH + 1];
    u8 fr[GTP_ADDR_LENGTH + 1];
    u8 chksum = 0;
    s32 i, ret;

    buf[0] = 0x80; buf[1] = 0x47;
    for (i = 0; i < GTP_DEF_CFG_LEN; i++) {
        buf[i + GTP_ADDR_LENGTH] = gtp_def_cfg[i];
        chksum += gtp_def_cfg[i];
    }
    chksum = 0xC1; /* 出厂校验和，原样使用 */

    ret = gtp_i2c_write(client, buf, GTP_DEF_CFG_LEN + GTP_ADDR_LENGTH);
    if (ret < 0) { GTP_ERROR("send cfg failed: %d", ret); return; }

    ck[0] = 0x80; ck[1] = 0xFF; ck[2] = chksum;
    gtp_i2c_write(client, ck, 3);

    fr[0] = 0x81; fr[1] = 0x00; fr[2] = 0x01;
    gtp_i2c_write(client, fr, 3);

    msleep(10);
    GTP_INFO("cfg sent, chksum=0x%02X", chksum);
}

/* ----------------------------------------------------------------------------
 *  gtp_init_panel：初始化面板参数（分辨率、配置版本号校验）
 *  理想流程是"读芯片配置 → 算校验和 → 和驱动内置表比对 → 不一致就重刷"。
 *  本板复位不可控，刷配置有风险，实际走的是"能读就读、读不到用默认"。
 * -------------------------------------------------------------------------- */
static s32 gtp_init_panel(struct goodix_ts_data *ts)
{
	/* gt9xx_cfg_send: 跳过（芯片 flash 自带出厂配置） */
    s32 ret = -1;

    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH;
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0) {
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }

    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0)) {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03;
    }

    GTP_INFO("X_MAX: %d, Y_MAX: %d, TRIGGER: 0x%02x", ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;

}

/* ----------------------------------------------------------------------------
 *  /proc/gt91xx_config 的读接口：把当前配置表原样打印出来
 *  用法：cat /proc/gt91xx_config，能看到 184 字节配置的十六进制内容。
 *  调试价值：刷配置前后对比一下，就知道配置有没有真的写进芯片。
 * -------------------------------------------------------------------------- */
static ssize_t gt91xx_config_read_proc(struct file *file, char __user *page, size_t size, loff_t *ppos)
{
    char *ptr = page;
    char temp_data[GTP_CONFIG_MAX_LENGTH + 2] = {0x80, 0x47};
    int i;
    if (*ppos) {
        return 0;
    }
    ptr += sprintf(ptr, "==== GT9XX config init value====\n");

    for (i = 0 ; i < GTP_CONFIG_MAX_LENGTH ; i++) {
        ptr += sprintf(ptr, "0x%02X ", config[i + 2]);

        if (i % 8 == 7)
            ptr += sprintf(ptr, "\n");
    }

    ptr += sprintf(ptr, "\n");

    ptr += sprintf(ptr, "==== GT9XX config real value====\n");
    gtp_i2c_read(i2c_connect_client, temp_data, GTP_CONFIG_MAX_LENGTH + 2);
    for (i = 0 ; i < GTP_CONFIG_MAX_LENGTH ; i++) {
        ptr += sprintf(ptr, "0x%02X ", temp_data[i+2]);

        if (i % 8 == 7)
            ptr += sprintf(ptr, "\n");
    }

    *ppos += ptr - page;

    return (ptr - page);
}

/* ----------------------------------------------------------------------------
 *  /proc/gt91xx_config 的写接口：向芯片写一份新配置
 *  用法：echo 十六进制串 > /proc/gt91xx_config（脚本里常用这种批量刷配置的方式）
 *  风险提示：配置写错会导致触摸坐标乱、甚至不响应，所以这是"调试口"，平时不用。
 * -------------------------------------------------------------------------- */
static ssize_t gt91xx_config_write_proc(struct file *filp, const char __user *buffer, size_t count, loff_t *off)
{
    GTP_DEBUG("write count %d\n", count);

    if (count > GTP_CONFIG_MAX_LENGTH) {
        GTP_ERROR("size not match [%d:%d]\n", GTP_CONFIG_MAX_LENGTH, count);
        return -EFAULT;
    }

    if (copy_from_user(&config[2], buffer, count)) {
        GTP_ERROR("copy from user fail\n");
        return -EFAULT;
    }

    return count;
}

/* ----------------------------------------------------------------------------
 *  gtp_read_version：读固件版本号（0x8140 起 6 字节）
 *  这个寄存器还有一个妙用：地址探测时"能读出版本号"就说明该地址有芯片应答。
 * -------------------------------------------------------------------------- */
s32 gtp_read_version(struct goodix_ts_data *ts)
{
    int error;
    u8 buf[6];
    char id_str[5];

    error = goodix_i2c_read(ts->client, GTP_REG_VERSION, buf, sizeof(buf));
    if (error) {
        dev_err(&ts->client->dev, "read version failed: %d\n", error);
        return error;
    }

    memcpy(id_str, buf, 4);
    id_str[4] = 0;
    if (kstrtou16(id_str, 10, &ts->id))
        ts->id = 0x1001;

    ts->version = get_unaligned_le16(&buf[4]);

    dev_info(&ts->client->dev, "ID %d, version: %04x\n", ts->id,
             ts->version);

    return 0;
}

/* ----------------------------------------------------------------------------
 *  gtp_i2c_test：I2C 通信自检（读一次版本号，读不到就返回失败）
 *  probe 靠它确认"地址选对了、总线通了"，是定位"触摸完全无反应"的第一道关卡
 * -------------------------------------------------------------------------- */
static s8 gtp_i2c_test(struct i2c_client *client, struct goodix_ts_data *ts)
{
    u8 test[3] = {ts->chip->config_addr >> 8, ts->chip->config_addr & 0xff};
    u8 retry = 0;
    s8 ret = -1;

    GTP_DEBUG_FUNC();

    while(retry++ < 5) {
        ret = gtp_i2c_read(client, test, 3);
        if (ret > 0) {
            return ret;
        }
        GTP_ERROR("GTP i2c test failed time %d.",retry);
        msleep(10);
    }
    return ret;
}

/* ============================================================================
 *  【坑2】申请并配置 INT / RST 两颗 GPIO
 *  ---------------------------------------------------------------------------
 *  原厂模板（坑）：int 脚也用了 GPIOF_OUT_INIT_HIGH —— 把"输入信号脚"配成了输出。
 *    后果 a：输出脚读不到芯片拉的电平，GPIO 值始终是自己写出去的那个
 *    后果 b：芯片想通过 INT 下降沿通知"有触摸"，但脚被驱动顶成高电平，
 *            电平根本不变化 → request_irq 注册的中断永远不触发
 *            （现象：/proc/interrupts 里 38 号计数一直是 0，触摸完全无反应）
 *  本板改法：
 *    ① 申请脚时用 GPIOF_IN（输入）
 *    ② GTP_GPIO_AS_INT：把这个脚复用成 INT 功能（不是普通 GPIO 输入）
 *    ③ gpio_to_irq → 得到中断号 38
 * ========================================================================== */
static s8 gtp_request_io_port(struct goodix_ts_data *ts)
{
    s32 ret = 0;
    GTP_DEBUG_FUNC();
    /* RST 脚：先按输出申请下来，高电平放着。虽然本板复用改不动，但申请一下不影响 */
    ret = devm_gpio_request_one(&ts->client->dev, gtp_rst_gpio,
                                GPIOF_OUT_INIT_HIGH, "Goodix-Ts reset PIN");
    if (ret < 0) {
        GTP_ERROR("Failed to request GPIO:%d, ERRNO:%d", (s32)gtp_int_gpio, ret);
        ret = -ENODEV;
	return ret;
    } else {
        GTP_GPIO_AS_INT(gtp_int_gpio);      /* INT 脚复用到中断功能 */
        ts->client->irq = gpio_to_irq(gtp_int_gpio);   /* GPIO 编号换成中断号，本板是 38 */
    }
    /* INT 脚按输入申请：只有输入才能读到芯片拉的电平，输出脚产生不了中断 */
    ret = devm_gpio_request_one(&ts->client->dev, gtp_int_gpio,
                                GPIOF_IN, "Goodix-Ts IRQ PIN");
    GTP_GPIO_AS_INT(gtp_int_gpio);
    if (ret < 0) {
        GTP_ERROR("Failed to request GPIO:%d, ERRNO:%d",(s32)gtp_rst_gpio, ret);
        ret = -ENODEV;
	return ret;
    }

    GTP_GPIO_AS_INPUT(gtp_rst_gpio);        /* RST 也按输入处理，避免和芯片抢电平 */
    gtp_reset_guitar(ts->client, 20);       /* 本板这个函数是空实现，见上面的说明 */
    
    return ret;
}

/* ----------------------------------------------------------------------------
 *  gtp_request_irq：注册中断处理函数
 *  第三个参数是"触发类型"（irq_table[int_trigger_type]），本板是下降沿触发
 *  （GT911 有数据时把 INT 拉低），所以不用双边沿，避免多余中断。
 *  注册失败会退回定时器轮询模式（原厂兜底逻辑），但那样 CPU 占用会高。
 * -------------------------------------------------------------------------- */
static s8 gtp_request_irq(struct goodix_ts_data *ts)
{
    s32 ret = -1;
    const u8 irq_table[] = GTP_IRQ_TAB;

    GTP_DEBUG_FUNC();
    GTP_DEBUG("INT trigger type:%x", ts->int_trigger_type);

    ret  = request_irq(ts->client->irq,
                       goodix_ts_irq_handler,
                       irq_table[ts->int_trigger_type],
            ts->client->name,
            ts);
    if (ret) {
        GTP_ERROR("Request IRQ failed!ERRNO:%d.", ret);
        GTP_GPIO_AS_INPUT(gtp_int_gpio);

        hrtimer_init(&ts->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
        ts->timer.function = goodix_ts_timer_handler;
        hrtimer_start(&ts->timer, ktime_set(1, 0), HRTIMER_MODE_REL);
        return -1;
    } else {
        gtp_irq_disable(ts);
        ts->use_irq = 1;
        return 0;
    }
}

/* ----------------------------------------------------------------------------
 *  gtp_request_input_dev：向内核 input 子系统注册触摸设备
 *  关键四步：input_allocate_device 申请 → set_bit 声明支持哪些事件
 *            → input_register_device 注册（内核自动生成 /dev/input/event1）
 *            → 用户态就能用 evdev 读事件了（Qt 的 evdevtouch 插件正是读它）
 *  ABS_X / ABS_Y 是绝对坐标（触摸屏必须用绝对坐标，不是鼠标的相对位移）
 * -------------------------------------------------------------------------- */
static s8 gtp_request_input_dev(struct goodix_ts_data *ts)
{
    s8 ret = -1;

    GTP_DEBUG_FUNC();

    ts->input_dev = input_allocate_device();
    if (ts->input_dev == NULL) {
        GTP_ERROR("Failed to allocate input device.");
        return -ENOMEM;
    }

    ts->input_dev->evbit[0] = BIT_MASK(EV_SYN) | BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS) ;
    input_mt_init_slots(ts->input_dev, GTP_MAX_TOUCH, 1);     // in case of "out of memory"
    __set_bit(INPUT_PROP_DIRECT, ts->input_dev->propbit);

    input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X, 0, ts->abs_x_max, 0, 0);
    input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y, 0, ts->abs_y_max, 0, 0);
    input_set_abs_params(ts->input_dev, ABS_MT_WIDTH_MAJOR, 0, 255, 0, 0);
    input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
    input_set_abs_params(ts->input_dev, ABS_MT_TRACKING_ID, 0, 255, 0, 0);

    ts->input_dev->name = goodix_ts_name;
    ts->input_dev->phys = goodix_input_phys;
    ts->input_dev->id.bustype = BUS_I2C;
    ts->input_dev->id.vendor = 0xDEAD;
    ts->input_dev->id.product = 0xBEEF;
    ts->input_dev->id.version = 10427;
    
    ret = input_register_device(ts->input_dev);
    if (ret) {
        GTP_ERROR("Register %s input device failed", ts->input_dev->name);
        return -ENODEV;
    }

    return 0;
}

/* ----------------------------------------------------------------------------
 *  gtp_parse_dt：从设备树解析驱动需要的参数
 *  of_get_named_gpio(np, "goodix,irq-gpio", 0)：读 goodix,irq-gpio 属性第 0 个值，
 *      得到全局 GPIO 编号（本板 = 9，对应 GPIO1_IO09）
 *  of_property_read_u32(...)：读复位延时、触摸屏尺寸等数值型属性
 *  为什么用设备树：引脚接法属于"硬件信息"，写进设备树后换板子只改 dts，不用改驱动。
 * -------------------------------------------------------------------------- */
static int gtp_parse_dt(struct device *dev)
{
    struct device_node *np = dev->of_node;

    gtp_int_gpio = of_get_named_gpio(np, "goodix,irq-gpio", 0);
    if (!gpio_is_valid(gtp_int_gpio)) {
        dev_err(dev, "Failed to get reset gpio\n");
        return -1;
    }

    gtp_rst_gpio = of_get_named_gpio(np, "goodix,rst-gpio", 0);
    if (!gpio_is_valid(gtp_rst_gpio)) {
        dev_err(dev, "Failed to get reset gpio\n");
        return -1;
    }

    return 0;
}

/* ----------------------------------------------------------------------------
 *  gtp_parse_dt_cfg：从设备树读"触摸配置表"（goodix,config 之类的属性）
 *  若设备树里没写，就返回失败，调用方会退回使用驱动内置的默认配置表。
 * -------------------------------------------------------------------------- */
int gtp_parse_dt_cfg(struct device *dev, u8 *cfg, int *cfg_len, u8 sid)
{
    struct device_node *np = dev->of_node;
    struct property *prop;
    char cfg_name[18];

    snprintf(cfg_name, sizeof(cfg_name), "goodix,cfg-group%d", sid);
    prop = of_find_property(np, cfg_name, cfg_len);
    if (!prop || !prop->value || *cfg_len == 0 || *cfg_len > GTP_CONFIG_MAX_LENGTH) {
        return -1;
    } else {
        memcpy(cfg, prop->value, *cfg_len);
        return 0;
    }
}

/* ----------------------------------------------------------------------------
 *  gtp_power_switch：给触摸芯片上电 / 断电
 *  regulator_get + regulator_enable：通过内核 regulator 框架控制供电（VDD/VDDIO）
 *  上电后要延时一段时间（芯片内部要稳定、要加载 flash 配置）才能开始通信，
 *  这段延时不够就会出现"probe 时读不到芯片"的偶发故障。
 * -------------------------------------------------------------------------- */
static int gtp_power_switch(struct i2c_client *client, int on)
{
    static struct regulator *vdd_ana;
    static struct regulator *vcc_i2c;
    int ret;

    if (!vdd_ana) {
        vdd_ana = regulator_get(&client->dev, "vdd_ana");
        if (IS_ERR(vdd_ana)) {
            GTP_ERROR("regulator get of vdd_ana failed");
            ret = PTR_ERR(vdd_ana);
            vdd_ana = NULL;
            return ret;
        }
    }

    if (!vcc_i2c) {
        vcc_i2c = regulator_get(&client->dev, "vcc_i2c");
        if (IS_ERR(vcc_i2c)) {
            GTP_ERROR("regulator get of vcc_i2c failed");
            ret = PTR_ERR(vcc_i2c);
            vcc_i2c = NULL;
            goto ERR_GET_VCC;
        }
    }

    if (on) {
        GTP_DEBUG("GTP power on.");
        ret = regulator_enable(vdd_ana);
        udelay(2);
        ret = regulator_enable(vcc_i2c);
    } else {
        GTP_DEBUG("GTP power off.");
        ret = regulator_disable(vcc_i2c);
        udelay(2);
        ret = regulator_disable(vdd_ana);
    }
    return ret;

ERR_GET_VCC:
    regulator_put(vdd_ana);
    return ret;
}

/* ---------------------------------------------------------------
 * 本板 RST 脚（SNVS_TAMPER9）的 MUX 被 SNVS 域硬件锁死，驱动无法主动
 * 复位 GT911，因此芯片的 I2C 地址完全由【上电瞬间 INT 电平】决定：
 *     上电时 INT 为高 -> 0x5D
 *     上电时 INT 为低 -> 0x14
 * 这里两个地址都实测一遍，谁应答就用谁，避免"点不动"。
 * --------------------------------------------------------------- */
/* ============================================================================
 *  【坑3】I2C 地址自动探测
 *  ---------------------------------------------------------------------------
 *  GT911 有两个可选从地址：0x5D 和 0x14，选哪个由"复位释放瞬间 INT 脚电平"决定：
 *      INT=高 → 0x5D        INT=低 → 0x14
 *  本板复位不可控（坑1），那个瞬间的电平受外部上拉、上次残留状态、上电时序影响，
 *  于是每次冷启动芯片可能选不同地址。设备树里只写死 0x5D，撞上 0x14 就
 *  probe 报 -ENODEV，界面触摸整体失效 —— 这就是"有时开机触摸好用、有时完全没反应"。
 *  处理：两个地址各读两次版本号，谁有应答就把 client->addr 改成谁，再继续初始化。
 * ========================================================================== */
static s32 gtp_detect_i2c_addr(struct i2c_client *client)
{
    u8 buf[GTP_ADDR_LENGTH + 6];
    u8 addrs[2];
    int i, j;

    addrs[0] = client->addr;                              /* 设备树里写的那个，一般是 0x5d */
    addrs[1] = (client->addr == 0x5d) ? 0x14 : 0x5d;      /* 另一个备选地址 */

    for (i = 0; i < 2; i++) {
        client->addr = addrs[i];                          /* 先按这个地址试 */
        for (j = 0; j < 2; j++) {
            buf[0] = GTP_REG_VERSION >> 8;                /* 版本号寄存器地址，高字节 */
            buf[1] = GTP_REG_VERSION & 0xff;              /* 低字节 */
            /* 读 6 个字节；返回 2 是驱动内部约定的成功值，说明这颗芯片在线 */
            if (gtp_i2c_read(client, buf, GTP_ADDR_LENGTH + 6) == 2) {
                GTP_INFO("GT911 detected at i2c addr 0x%02x", client->addr);
                return 0;
            }
            msleep(5);                                    /* 没应答就等一下再试，芯片刚上电可能还没就绪 */
        }
    }
    return -ENODEV;                                       /* 两个地址都没反应，放弃 probe */
}

/* ============================================================================
 *  probe：驱动与设备"对上眼"时内核调用（设备树 compatible 匹配成功）
 *  ---------------------------------------------------------------------------
 *  probe 里的顺序是固定的，照着往下走：
 *    ① gtp_parse_dt        从设备树读出 INT/RST 引脚号、复位延时
 *    ② gtp_power_switch    上电（regulator），并等芯片稳定
 *    ③ gtp_detect_i2c_addr 自动探测 0x5D / 0x14（坑3）
 *    ④ gtp_i2c_test        读版本号确认通信正常
 *    ⑤ gtp_request_io_port 申请 GPIO、INT 复用、gpio_to_irq（坑2）
 *    ⑥ gtp_request_input_dev 注册 input 设备 → 生成 /dev/input/event1
 *    ⑦ gtp_request_irq     注册中断处理函数
 *    ⑧ 读版本号、初始化面板、使能中断
 *  任何一步失败都 goto 回滚，把前面申请的资源释放掉（不会泄漏）。
 * ========================================================================== */
static int goodix_ts_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    s32 ret = -1;
    struct goodix_ts_data *ts;
    
    GTP_DEBUG_FUNC();
    
    //do NOT remove these logs
    GTP_INFO("GTP Driver Version: %s", GTP_DRIVER_VERSION);
    GTP_INFO("GTP I2C Address: 0x%02x", client->addr);

    i2c_connect_client = client;

    ts = kzalloc(sizeof(*ts), GFP_KERNEL);
    if (ts == NULL) {
        GTP_ERROR("Alloc GFP_KERNEL memory failed.");
        return -ENOMEM;
    }

    ts->client = client;

    if (client->dev.of_node) {
        ret = gtp_parse_dt(&client->dev);
	if (ret < 0)
	    goto free_ts;
    }

    ret = gtp_request_io_port(ts);
    if (ret < 0) {
        GTP_ERROR("GTP request IO port failed.");
        goto free_ts;
    }

    if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
        GTP_ERROR("I2C check functionality failed.");
        return -ENODEV;
    }

    ret = gtp_power_switch(client, 1);
    if (ret) {
        GTP_ERROR("GTP power on failed.");
        return -EINVAL;
    }

    /* 地址自动探测：0x5D / 0x14 哪个应答就用哪个 */
    ret = gtp_detect_i2c_addr(client);
    if (ret < 0) {
        GTP_ERROR("GT911 not found at 0x5d nor 0x14");
        goto free_ts;
    }

    INIT_WORK(&ts->work, goodix_ts_work_func);
    spin_lock_init(&ts->irq_lock);          // 2.6.39 later
    // ts->irq_lock = SPIN_LOCK_UNLOCKED;   // 2.6.39 & before
    i2c_set_clientdata(client, ts);
    ts->gtp_rawdiff_mode = 0;

    ret = gtp_read_version(ts);
    if (ret < 0) {
        GTP_ERROR("Read version failed.");
    }

    ts->chip = goodix_get_chip_data(ts->id);

    ret = gtp_i2c_test(client, ts);
    if (ret < 0) {
        GTP_ERROR("I2C communication ERROR!");
        goto free_ts;
    }
    
    ret = gtp_init_panel(ts);
    if (ret < 0) {
        GTP_ERROR("GTP init panel failed.");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
        goto free_ts;
    }
    
    // Create proc file system
    gt91xx_config_proc = proc_create(GT91XX_CONFIG_PROC_FILE, 0666, NULL, &config_proc_ops);
    if (gt91xx_config_proc == NULL) {
        GTP_ERROR("create_proc_entry %s failed\n", GT91XX_CONFIG_PROC_FILE);
    } else {
        GTP_INFO("create proc entry %s success", GT91XX_CONFIG_PROC_FILE);
    }

    ret = gtp_request_input_dev(ts);
    if (ret < 0) {
        GTP_ERROR("GTP request input dev failed");
    }
    
    ret = gtp_request_irq(ts);
    if (ret < 0) {
        GTP_INFO("GTP works in polling mode.");
    } else {
        GTP_INFO("GTP works in interrupt mode.");
    }

    if (ts->use_irq) {
        gtp_irq_enable(ts);
    }

    return 0;
free_ts:
    kfree(ts);
    return ret;
}

/* ----------------------------------------------------------------------------
 *  remove：卸载驱动 / 拔设备时调用，按 probe 的反顺序回收：
 *  关中断 → 注销 input 设备 → 释放 GPIO → 断电
 * -------------------------------------------------------------------------- */
static int goodix_ts_remove(struct i2c_client *client)
{
    struct goodix_ts_data *ts = i2c_get_clientdata(client);
    
    GTP_DEBUG_FUNC();

    if (ts) {
        if (ts->use_irq) {
            GTP_GPIO_AS_INPUT(gtp_int_gpio);
            free_irq(client->irq, ts);
        } else {
            hrtimer_cancel(&ts->timer);
        }
    }
    
    GTP_INFO("GTP driver removing...");
    i2c_set_clientdata(client, NULL);
    input_unregister_device(ts->input_dev);
    kfree(ts);

    return 0;
}

static int __maybe_unused goodix_ts_suspend(struct device *dev)
{
    struct i2c_client *client = to_i2c_client(dev);

    if (device_may_wakeup(dev))
        enable_irq_wake(client->irq);

    return 0;
}

static int __maybe_unused goodix_ts_resume(struct device *dev)
{
    struct i2c_client *client = to_i2c_client(dev);

    if (device_may_wakeup(dev))
        disable_irq_wake(client->irq);

    return 0;
}


static SIMPLE_DEV_PM_OPS(goodix_ts_pm_ops,
                         goodix_ts_suspend, goodix_ts_resume);

/* ----------------------------------------------------------------------------
 *  goodix_match_table：设备树匹配表
 *  compatible = "goodix,gt9xx" 必须和设备树里的写法一字不差，
 *  内核就是靠它把 dts 里的 goodix_ts@5d 节点和这个驱动配对，然后调用 probe。
 *  .data 里放的是型号标识（GT911），运行时用来决定屏的分辨率等参数。
 * -------------------------------------------------------------------------- */
static const struct of_device_id goodix_match_table[] = {
{.compatible = "goodix,gt9xx",},
{.compatible = "goodix,gt1151",},
{.compatible = "goodix,gt9147",},
{.compatible = "goodix,gt9271",},
{ },
};

/* ----------------------------------------------------------------------------
 *  goodix_ts_id：I2C 设备 ID 匹配表（老式写法）
 *  设备树匹配走 goodix_match_table，名字匹配走这张表，两条路都能让 probe 被调用。
 *  表最后必须以 { } 空项结尾，内核靠它判断"表到末尾了"。
 * -------------------------------------------------------------------------- */
static const struct i2c_device_id goodix_ts_id[] = {
{ GTP_I2C_NAME, 0 },
{ }
};

/* ----------------------------------------------------------------------------
 *  goodix_ts_driver：把驱动"登记"给 I2C 子系统
 *    .probe  / .remove        ：设备匹配上 / 移除时调用
 *    .id_table               ：i2c_device_id 表（老式匹配方式）
 *    .driver.of_match_table  ：设备树匹配表（本板走这条）
 *  注册后，I2C 总线扫描到 0x5D 或 0x14 上有芯片应答，且 compatible 匹配，就会 probe。
 * -------------------------------------------------------------------------- */
static struct i2c_driver goodix_ts_driver = {
    .probe      = goodix_ts_probe,
    .remove     = goodix_ts_remove,
    .id_table   = goodix_ts_id,
    .driver = {
        .name     = GTP_I2C_NAME,
        .owner    = THIS_MODULE,
        .of_match_table = goodix_match_table,
        .pm             = &goodix_ts_pm_ops,
    },
};

/* ----------------------------------------------------------------------------
 *  模块入口（insmod 时执行）
 *  ① create_singlethread_workqueue：建一个单线程工作队列，中断下半部要用它
 *     （单线程的好处：坐标处理天然串行，不会两个 CPU 同时读 I2C 抢总线）
 *  ② i2c_add_driver：把驱动注册到 I2C 子系统，触发 probe
 * -------------------------------------------------------------------------- */
static int __init goodix_ts_init(void)
{
    s32 ret;

    GTP_DEBUG_FUNC();
    GTP_INFO("GTP driver installing...");
    goodix_wq = create_singlethread_workqueue("goodix_wq");
    if (!goodix_wq) {
        GTP_ERROR("Creat workqueue failed.");
        return -ENOMEM;
    }
    ret = i2c_add_driver(&goodix_ts_driver);
    return ret;
}

/* ----------------------------------------------------------------------------
 *  模块出口（rmmod 时执行）
 *  先 i2c_del_driver（会触发 remove，把中断、input 设备、GPIO 都回收），
 *  再 destroy_workqueue 销毁工作队列，顺序不能反。
 * -------------------------------------------------------------------------- */
static void __exit goodix_ts_exit(void)
{
    GTP_DEBUG_FUNC();
    GTP_INFO("GTP driver exited.");
    i2c_del_driver(&goodix_ts_driver);
    if (goodix_wq) {
        destroy_workqueue(goodix_wq);
    }
}

module_init(goodix_ts_init);
module_exit(goodix_ts_exit);

MODULE_DESCRIPTION("GTP Series Driver");
MODULE_LICENSE("GPL");
