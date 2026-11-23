/*
 * chrdevbase.c —— i.MX6ULL 字符设备驱动
 * 平台：正点原子 ALPHA V2.2（i.MX6ULL eMMC 版），内核 Linux 4.1.15
 *
 * 一个主设备号 + 三个次设备号，三路硬件各占一个节点：
 *   249,0 → /dev/led   写 '1' 开灯、'0' 关灯
 *   249,1 → /dev/beep  写 'b' 响、'B' 停
 *   249,2 → /dev/key   中断记事件，read 取电平，poll 做通知
 *
 * 三个节点由同一个 .ko 提供，共用 GPIO 与一个 /proc 调试接口。
 * 主设备号动态申请，所以开机脚本按设备名现取主号再建节点。
 */

/* 内核头文件：模块宏、设备号与 cdev、用户态拷贝、GPIO、中断、等待队列、poll、proc */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/kdev_t.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

/* GPIO 全局编号 = (组号 - 1) * 32 + 组内序号 */
#define IMX_GPIO_NR(group, num)     (((group) - 1) * 32 + (num))

/* 三路硬件引脚，均为低电平有效 */
#define LED_GPIO      IMX_GPIO_NR(1, 3)     /* GPIO1_IO03 */
#define BEEP_GPIO     IMX_GPIO_NR(5, 1)     /* GPIO5_IO01 */
#define KEY0_GPIO     IMX_GPIO_NR(1, 18)    /* GPIO1_IO18，按下为低 */

#define CHRDEVBASE_NAME   "chrdevbase"

/* 次设备号：主号只有一个，靠次号区分是哪一路硬件 */
#define LED_MINOR     0
#define BEEP_MINOR    1
#define KEY_MINOR     2
#define DEV_COUNT     3

static dev_t            chrdev_dev;     /* 号段起点 */
static int              dev_major;      /* 主设备号，打印日志与建节点时用 */
static struct cdev      led_cdev;       /* 一个 cdev 绑一个次设备号 */
static struct cdev      beep_cdev;
static struct cdev      key_cdev;

/*
 * 按键状态：中断里写，read / poll 里读。
 * key_seq 是事件序号，电平每变一次加一；key_read_seq 是已被读走的序号，
 * 两者相等表示没有新事件，read 是否阻塞、poll 是否就绪都靠这一对变量判断。
 */
static int               key_irq;
static volatile char     key_level = 1; /* 1 松开、0 按下 */
static unsigned int      key_press_cnt;
static unsigned int      key_seq;
static unsigned int      key_read_seq;
static wait_queue_head_t key_wq;        /* read 与 poll 共同的等待队列 */

static int led_state;                   /* 仅供 /proc 查看 */
static int beep_state;

/*
 * 按键中断服务程序（上半部）。
 * 中断上下文不能睡眠、不能访问用户态内存，所以这里只做三件事：
 * 读电平、更新计数与序号、唤醒等待队列；把数据交给用户态的活留给 read。
 */
static irqreturn_t key_irq_handler(int irq, void *dev_id)
{
    int level = gpio_get_value(KEY0_GPIO);  /* 读引脚真实电平 */

    if (level != key_level) {           /* 电平没变视为抖动，跳过 */
        key_level = (char)level;
        if (level == 0)
            key_press_cnt++;            /* 只在按下时累加次数 */
        key_seq++;                      /* 表示有新事件待读 */
        wake_up_interruptible(&key_wq);
    }

    return IRQ_HANDLED;
}

/* ---------------- 次设备号 0：LED ---------------- */

/* 打开 /dev/led：GPIO 在模块加载时已配好，这里不做动作 */
static int led_open(struct inode *inode, struct file *filp)
{
    return 0;
}

/* 关闭 /dev/led：灯的状态要保持，无资源可释放 */
static int led_release(struct inode *inode, struct file *filp)
{
    return 0;
}

/*
 * LED 写函数：'1' 点亮、'0' 熄灭，其它字符返回 -EINVAL。
 * 参数：buf 来自用户态，count 为字节数；成功返回写入长度。
 */
static ssize_t led_write(struct file *filp, const char __user *buf,
                         size_t count, loff_t *ppos)
{
    char val;

    if (count < 1)
        return -EINVAL;

    if (copy_from_user(&val, buf, 1))   /* 从用户空间拷 1 字节进内核 */
        return -EFAULT;

    switch (val) {
    case '1':
        gpio_set_value(LED_GPIO, 0);    /* 低电平点亮 */
        led_state = 1;
        break;
    case '0':
        gpio_set_value(LED_GPIO, 1);
        led_state = 0;
        break;
    default:
        printk("chrdevbase: led node got unknown cmd 0x%02x\n", val);
        return -EINVAL;
    }

    return count;
}

/* ---------------- 次设备号 1：蜂鸣器 ---------------- */

static int beep_open(struct inode *inode, struct file *filp)
{
    return 0;
}

static int beep_release(struct inode *inode, struct file *filp)
{
    return 0;
}

/* 蜂鸣器写函数：'b' 响、'B' 停，其余字符报错 */
static ssize_t beep_write(struct file *filp, const char __user *buf,
                          size_t count, loff_t *ppos)
{
    char val;

    if (count < 1)
        return -EINVAL;

    if (copy_from_user(&val, buf, 1))
        return -EFAULT;

    switch (val) {
    case 'b':
        gpio_set_value(BEEP_GPIO, 0);   /* 低电平导通三极管 */
        beep_state = 1;
        break;
    case 'B':
        gpio_set_value(BEEP_GPIO, 1);
        beep_state = 0;
        break;
    default:
        printk("chrdevbase: beep node got unknown cmd 0x%02x\n", val);
        return -EINVAL;
    }

    return count;
}

/* ---------------- 次设备号 2：KEY0 按键 ---------------- */

static int key_open(struct inode *inode, struct file *filp)
{
    return 0;
}

static int key_release(struct inode *inode, struct file *filp)
{
    return 0;
}

/*
 * 按键读函数：把当前电平（0 按下 / 1 松开）交给用户态。
 * 没有新事件时，带 O_NONBLOCK 立即返回 -EAGAIN，否则睡在 key_wq 上等中断唤醒；
 * 被信号打断返回 -ERESTARTSYS。
 */
static ssize_t key_read(struct file *filp, char __user *buf,
                        size_t count, loff_t *ppos)
{
    unsigned char val;

    if (count == 0)
        return 0;

    if (key_seq == key_read_seq) {      /* 序号相等 = 没有未读事件 */
        if (filp->f_flags & O_NONBLOCK)
            return -EAGAIN;

        if (wait_event_interruptible(key_wq, key_seq != key_read_seq))
            return -ERESTARTSYS;
    }

    key_read_seq = key_seq;             /* 标记该事件已被消费 */
    val = (unsigned char)key_level;

    if (copy_to_user(buf, &val, 1))
        return -EFAULT;

    return 1;
}

/*
 * poll 回调：select / poll / epoll 用它判断节点是否可读，界面里的 QSocketNotifier 也走这里。
 * poll_wait 只登记等待队列，不睡眠；有未读事件时置 POLLIN | POLLPRI。
 */
static unsigned int key_poll(struct file *filp, poll_table *wait)
{
    unsigned int mask = 0;

    poll_wait(filp, &key_wq, wait);

    if (key_seq != key_read_seq)
        mask |= POLLIN | POLLPRI;

    return mask;
}

/* 三个节点三张操作表：led / beep 只用 write，key 用 read 与 poll */
static struct file_operations led_fops = {
    .owner   = THIS_MODULE,             /* 引用计数，节点被占用时模块不能卸载 */
    .open    = led_open,
    .release = led_release,
    .write   = led_write,
};

static struct file_operations beep_fops = {
    .owner   = THIS_MODULE,
    .open    = beep_open,
    .release = beep_release,
    .write   = beep_write,
};

static struct file_operations key_fops = {
    .owner   = THIS_MODULE,
    .open    = key_open,
    .release = key_release,
    .read    = key_read,
    .poll    = key_poll,
};

/*
 * /proc 调试节点：cat /proc/chrdevbase_key 可看到按下次数、当前电平、
 * 灯与蜂鸣器状态以及按键中断号。
 */
static int key_proc_show(struct seq_file *m, void *v)
{
    seq_printf(m, "press=%u level=%d led=%d beep=%d irq=%d\n",
               key_press_cnt, key_level, led_state, beep_state, key_irq);
    return 0;
}

static int key_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, key_proc_show, NULL);  /* seq_file 的单次输出封装 */
}

static const struct file_operations key_proc_fops = {
    .owner   = THIS_MODULE,
    .open    = key_proc_open,
    .read    = seq_read,
    .llseek  = seq_lseek,
    .release = single_release,
};

/*
 * 模块加载：insmod 时执行，__init 段加载完即释放。
 * 顺序：申请三路 GPIO 并配方向 → 初始化等待队列 → 取中断号并注册中断
 *       → 申请号段 → 三个 cdev_add → 建 /proc 节点。
 * 任何一步失败都 goto 到对应标签，按反向顺序回收已申请资源。
 */
static int __init chrdevbase_init(void)
{
    int ret;

    printk("chrdevbase_init\n");

    ret = gpio_request(LED_GPIO, "led_gpio");   /* 申请引脚所有权 */
    if (ret < 0) {                      /* 失败一般是引脚已被别的驱动占用 */
        printk("gpio_request LED_GPIO failed!\n");
        return ret;
    }
    gpio_direction_output(LED_GPIO, 1); /* 输出高电平，灯灭 */
    led_state = 0;

    ret = gpio_request(BEEP_GPIO, "beep_gpio");
    if (ret < 0) {
        printk("gpio_request BEEP_GPIO failed!\n");
        gpio_free(LED_GPIO);
        return ret;
    }
    gpio_direction_output(BEEP_GPIO, 1);
    beep_state = 0;

    ret = gpio_request(KEY0_GPIO, "key0_gpio");
    if (ret < 0) {
        printk("gpio_request KEY0_GPIO failed!\n");
        gpio_free(BEEP_GPIO);
        gpio_free(LED_GPIO);
        return ret;
    }
    gpio_direction_input(KEY0_GPIO);    /* 按键是输入 */
    key_level = (char)gpio_get_value(KEY0_GPIO);  /* 记录初始电平 */

    init_waitqueue_head(&key_wq);

    key_irq = gpio_to_irq(KEY0_GPIO);   /* GPIO 编号换成中断号，本板 47 */
    if (key_irq < 0) {
        printk("gpio_to_irq failed: %d\n", key_irq);
        ret = key_irq;
        goto err_gpio;
    }

    /* 双边沿触发：按下与松开都要感知到 */
    ret = request_irq(key_irq, key_irq_handler,
                      IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
                      "key0_irq", NULL);
    if (ret < 0) {
        printk("request_irq failed: %d\n", ret);
        goto err_gpio;
    }

    /* 动态申请 1 个主设备号 + 3 个连续次设备号，避免撞号 */
    ret = alloc_chrdev_region(&chrdev_dev, 0, DEV_COUNT, CHRDEVBASE_NAME);
    if (ret < 0) {
        printk("alloc_chrdev_region failed: %d\n", ret);
        goto err_irq;
    }
    dev_major = MAJOR(chrdev_dev);

    /* 三个 cdev 分别绑定 0/1/2 三个次设备号 */
    cdev_init(&led_cdev, &led_fops);
    led_cdev.owner = THIS_MODULE;
    ret = cdev_add(&led_cdev, MKDEV(dev_major, LED_MINOR), 1);
    if (ret < 0) {
        printk("cdev_add led failed: %d\n", ret);
        goto err_region;
    }

    cdev_init(&beep_cdev, &beep_fops);
    beep_cdev.owner = THIS_MODULE;
    ret = cdev_add(&beep_cdev, MKDEV(dev_major, BEEP_MINOR), 1);
    if (ret < 0) {
        printk("cdev_add beep failed: %d\n", ret);
        goto err_cdev_led;
    }

    cdev_init(&key_cdev, &key_fops);
    key_cdev.owner = THIS_MODULE;
    ret = cdev_add(&key_cdev, MKDEV(dev_major, KEY_MINOR), 1);
    if (ret < 0) {
        printk("cdev_add key failed: %d\n", ret);
        goto err_cdev_beep;
    }

    proc_create("chrdevbase_key", 0, NULL, &key_proc_fops);  /* 调试用，失败不影响主功能 */

    printk("chrdevbase: major=%d minors=%d/%d/%d irq=%d ready\n",
           dev_major, LED_MINOR, BEEP_MINOR, KEY_MINOR, key_irq);
    return 0;

/*
 * 出错回收区：标签从上往下执行，顺序与申请顺序相反，
 * 跳到哪一层就把那一层之后申请的资源全部还掉。
 */
err_cdev_beep:
    cdev_del(&beep_cdev);
err_cdev_led:
    cdev_del(&led_cdev);
err_region:
    unregister_chrdev_region(chrdev_dev, DEV_COUNT);
err_irq:
    free_irq(key_irq, NULL);
err_gpio:
    gpio_free(KEY0_GPIO);
    gpio_free(BEEP_GPIO);
    gpio_free(LED_GPIO);
    return ret;
}

/*
 * 模块卸载：rmmod 时执行，按加载的反向顺序释放。
 * 先把外设恢复到安全状态（灯灭、蜂鸣器停），再还引脚。
 */
static void __exit chrdevbase_exit(void)
{
    remove_proc_entry("chrdevbase_key", NULL);

    cdev_del(&key_cdev);
    cdev_del(&beep_cdev);
    cdev_del(&led_cdev);
    unregister_chrdev_region(chrdev_dev, DEV_COUNT);

    free_irq(key_irq, NULL);

    gpio_set_value(LED_GPIO, 1);
    gpio_free(LED_GPIO);

    gpio_set_value(BEEP_GPIO, 1);
    gpio_free(BEEP_GPIO);

    gpio_free(KEY0_GPIO);

    printk("chrdevbase_exit\n");
}

module_init(chrdevbase_init);           /* 加载入口 */
module_exit(chrdevbase_exit);           /* 卸载入口 */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("student");
MODULE_DESCRIPTION("i.MX6ULL chrdevbase: 1 major + 3 minors (LED / BEEP / KEY0)");
