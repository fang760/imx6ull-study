/*
 * keyAPP.c —— 读按键事件的命令行测试程序
 *
 * 用法：./keyAPP /dev/key
 * 每读到一个事件打印一行：0 按下、1 松开，Ctrl+C 退出。
 *
 * 读的是次设备号 2 对应的节点，用来验证“中断 → 等待队列 → read”这条链路。
 */
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

/*
 * 这里用阻塞方式打开（不带 O_NONBLOCK）：没有按键时 read 会睡在驱动的等待队列上，
 * 不占 CPU；按键中断里 wake_up_interruptible 一唤醒，read 立刻返回电平值。
 */
int main(int argc, char *argv[])
{
    int fd;
    unsigned char val;

    if (argc != 2) {
        printf("Usage: %s /dev/key\n", argv[0]);
        return -1;
    }

    fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        perror("open failed");
        return -1;
    }

    while (1) {
        read(fd, &val, 1);              /* 没有新事件时阻塞在这里 */

        if (val == 0)
            printf("KEY0 pressed  (val=0)\r\n");
        else
            printf("KEY0 released (val=1)\r\n");

        usleep(200000);                 /* 200ms 去抖，避免一次按下打印多行 */
    }

    close(fd);
    return 0;
}
