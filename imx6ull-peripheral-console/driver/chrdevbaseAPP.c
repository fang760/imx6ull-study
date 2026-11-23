/*
 * chrdevbaseAPP.c —— 点灯 / 开关蜂鸣器的命令行测试程序
 *
 * 用法：./chrdevbaseAPP <设备节点> <命令字符>
 *   /dev/led  1 | 0    开灯 / 关灯
 *   /dev/beep b | B    响 / 停
 *
 * 作用：不依赖界面，直接验证驱动和硬件是否正常，出问题时先跑它来定位。
 */
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>

/*
 * argc 为参数个数，argv 为参数字符串数组。
 * 例如 ./chrdevbaseAPP /dev/led 1 时：argv[1] 是节点，argv[2] 是命令字符。
 */
int main(int argc, char *argv[])
{
    int fd;
    char w_buf[2];
    int ret;

    if (argc != 3) {
        printf("Error usage!\n");
        printf("Usage: ./chrdevbaseAPP <dev> <cmd>\n");
        printf("  /dev/led   1 : led on\n");
        printf("  /dev/led   0 : led off\n");
        printf("  /dev/beep  b : beep on\n");
        printf("  /dev/beep  B : beep off\n");
        return -1;
    }

    fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        perror("open failed");
        return -1;
    }

    w_buf[0] = argv[2][0];              /* 命令只取第一个字符 */

    ret = write(fd, w_buf, 1);          /* 写 1 字节，驱动据此操作 GPIO */
    if (ret < 0) {
        perror("write failed");
    }

    close(fd);
    return 0;
}
