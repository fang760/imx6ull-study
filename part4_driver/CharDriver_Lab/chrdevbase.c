#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/ide.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/uaccess.h>



#define CHRDEVBASE_MAJOR        200         //主设备号200
#define CHRDEVBASE_NAME     "chrdevbase"    //名字

static char readbuf[100];   //读缓冲
static char writebuf[100];  //写缓冲
static char kerneldata[]={"kernel data!"};


static int chrdevbase_open(struct inode *inode,struct file *filp)
{
    printk("chrdevbase_open\r\n");
    return 0;
}
static ssize_t chrdevbase_read(struct file *filp,char __user *buf,size_t count,loff_t *offt)
{
    int ret =0;

    //向用户空间发送数据
    memcpy(readbuf,kerneldata,sizeof(kerneldata));
    ret=copy_to_user(buf,readbuf,count);
    if(ret==0){

    }else
    {
        
    }
    //printk("chrdevbase_read\r\n");
    return 0;
}
static int chrdevbase_release(struct inode *inode,struct file *filp)
{
    printk("chrdevbase_release\r\n");
    return 0;
}

static ssize_t chrdevbase_write(struct file *filp,const char __user *buf,size_t count,loff_t *ppos)
{
    int ret=0;
    ret = copy_from_user(writebuf,buf,count);
    if(ret==0){
        printk("kernel recevdata:%s\r\n",writebuf);
    }else{

    }
    //printk("chrdevbase_write\r\n");
    return 0;
}

/*
*字符设备操作集合
*/
static struct file_operations chrdevbase_fops={
    .owner = THIS_MODULE,
    .open = chrdevbase_open,
    .release = chrdevbase_release,
    .read = chrdevbase_read,
    .write = chrdevbase_write,
};

/*模块入口与出口*/

static int __init chrdevbase_init(void)
{
    int ret = 0;
    printk("chrdevbase_init\r\n");

    //注册字符设备
    ret = register_chrdev(CHRDEVBASE_MAJOR, CHRDEVBASE_NAME, &chrdevbase_fops);
    if(ret<0){
        printk("chrdevbase init failed!\r\n");
    }
    return 0;
}

static void __exit chrdevbase_exit(void)
{
    printk("chrdevbase_exit\r\n");
    //注销字符设备
    unregister_chrdev(CHRDEVBASE_MAJOR,CHRDEVBASE_NAME);
}
module_init(chrdevbase_init);      //入口//
module_exit(chrdevbase_exit);      //出口//

MODULE_LICENSE("GPL");
MODULE_AUTHOR("fxh");

