#include "asm-generic/errno-base.h"
#include <linux/printk.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/gpio.h>
#include <linux/mm.h>
#include <linux/cdev.h>
#include <linux/seq_file.h>
#include <linux/interrupt.h>
#include <linux/highmem.h>
#include <linux/version.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/kernel.h>
#include <linux/slab.h> // For kmalloc, kfree
#include <linux/vmalloc.h> // For vmalloc, vfree
#include <linux/gfp.h>
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 3, 0)
#include <asm/switch_to.h>
#else
#include <asm/system.h>
#endif

#include "ring_buffer.h"
