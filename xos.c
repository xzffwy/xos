
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/miscdevice.h>
#include <linux/compat.h>
#include <linux/fs.h>
#include <asm/uaccess.h>

#include "vmx.h"
MODULE_LICENSE("GPL");



static struct miscdevice misc = {
    254,
	"dune",
};

static int __init xos_init(void)
{

	int r;
    if((r=vmx_init())){
		printk(KERN_ERR "hehe:failed to initialize vmx\n");
		return r;
	}
	r = misc_register(&misc);
    printk(KERN_ALERT "hehe");
	if (r) {
		printk(KERN_ERR " misc device register failed\n");
	}

	return r;
}

static void __exit xos_exit(void)
{
	misc_deregister(&misc);
    vmx_exit();
}


module_init(xos_init);
module_exit(xos_exit);
