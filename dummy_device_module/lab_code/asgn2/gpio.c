/**
 * File: gpio.c
 * Date: 02/07/2021
 * Author: Zhiyi Huang
 * Version: 0.3
 *
 * This is a gpio API for the dummy gpio device which
 * generates an interrupt for each half-byte (the most significant
 * bits are generated first.
 *
 * COSC440 assignment 2 in 2021.
 */

/* This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */
#include "linux/mutex.h"
#define BUF_SIZE 2000
#define MYDEV_NAME "asgn2"
#define MY_PROC_NAME "asgn2_proc"
#define MYIOC_TYPE 'k'
#include <linux/printk.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/list.h>
#include <linux/gpio.h>
#include <linux/mm.h>
#include <linux/cdev.h>
#include <linux/seq_file.h>
#include <linux/highmem.h>
#include <linux/interrupt.h>
#include <linux/version.h>
#include <linux/delay.h>
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 3, 0)
#include <asm/switch_to.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h> // For kmalloc, kfree
#include <linux/vmalloc.h> // For vmalloc, vfree
#include <linux/gfp.h>
#else
#include <asm/system.h>
#endif

#include "ring_buffer.h"
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jake Norton");
MODULE_DESCRIPTION("COSC440 asgn2");

//including like this for now
#include "device_operations.c"
#define BCM2835_PERI_BASE 0x3f000000

static u32 gpio_dummy_base;

/* Define GPIO pins for the dummy device 
 * The pin numbers are attained from "cat /sys/kernel/debug/gpio"
 * */
static struct gpio gpio_dummy[] = {
	{ 519, GPIOF_IN, "GPIO7" },
	{ 520, GPIOF_OUT_INIT_HIGH, "GPIO8" },
	{ 529, GPIOF_IN, "GPIO17" },
	{ 530, GPIOF_OUT_INIT_HIGH, "GPIO18" },
	{ 534, GPIOF_IN, "GPIO22" },
	{ 535, GPIOF_OUT_INIT_HIGH, "GPIO23" },
	{ 536, GPIOF_IN, "GPIO24" },
	{ 537, GPIOF_OUT_INIT_HIGH, "GPIO25" },
	{ 516, GPIOF_OUT_INIT_LOW, "GPIO4" },
	{ 539, GPIOF_IN, "GPIO27" },
};
static int dummy_irq;

extern irqreturn_t dummyport_interrupt(int irq, void *dev_id);

static inline u32 gpio_inw(u32 addr)
{
	u32 data;

	asm volatile("ldr %0,[%1]" : "=r"(data) : "r"(addr));
	return data;
}

static inline void gpio_outw(u32 addr, u32 data)
{
	asm volatile("str %1,[%0]" : : "r"(addr), "r"(data));
}

void setgpiofunc(u32 func, u32 alt)
{
	u32 sel, data, shift;

	if (func > 53)
		return;
	sel = 0;
	while (func > 10) {
		func = func - 10;
		sel++;
	}
	sel = (sel << 2) + gpio_dummy_base;
	data = gpio_inw(sel);
	shift = func + (func << 1);
	data &= ~(7 << shift);
	data |= alt << shift;
	gpio_outw(sel, data);
}

u8 read_half_byte(void)
{
	u32 c;
	u8 r;

	r = 0;
	c = gpio_inw(gpio_dummy_base + 0x34);
	if (c & (1 << 7))
		r |= 1;
	if (c & (1 << 17))
		r |= 2;
	if (c & (1 << 22))
		r |= 4;
	if (c & (1 << 24))
		r |= 8;

	return r;
}

static void write_to_gpio(char c)
{
	volatile unsigned *gpio_set, *gpio_clear;

	gpio_set = (unsigned *)((char *)gpio_dummy_base + 0x1c);
	gpio_clear = (unsigned *)((char *)gpio_dummy_base + 0x28);

	if (c & 1)
		*gpio_set = 1 << 8;
	else
		*gpio_clear = 1 << 8;
	udelay(1);

	if (c & 2)
		*gpio_set = 1 << 18;
	else
		*gpio_clear = 1 << 18;
	udelay(1);

	if (c & 4)
		*gpio_set = 1 << 23;
	else
		*gpio_clear = 1 << 23;
	udelay(1);

	if (c & 8)
		*gpio_set = 1 << 25;
	else
		*gpio_clear = 1 << 25;
	udelay(1);
}

static u8 one_byte = 0;
static bool first_half = true;
DECLARE_RINGBUFFER(ring_buffer);

// Copies data to circular dynamic linked list one byte at a time
static void copy_to_mem_list(unsigned long t_arg)
{
	smp_mb(); // Full memory barrier
	char new_char = ringbuffer_read(&ring_buffer);
	asgn2_device.data_size++;

	if (new_char == '\0') {
		pr_err("END OF WRITE");
		new_char = SENTINEL;
		atomic_inc(&asgn2_device.file_count);
	}

	d_list_write(&asgn2_device.dlist, new_char);

	smp_mb(); // Full memory barrier
}

static DECLARE_TASKLET_OLD(circular_tasklet, copy_to_mem_list);

irqreturn_t dummyport_interrupt(int irq, void *dev_id)
{
	u8 half = read_half_byte();

	if (first_half) {
		one_byte = half
			   << 4; // Store the first half in the upper 4 bits
		first_half = false;
	} else {
		one_byte |=
			half; // Combine with the second half in the lower 4 bits
		first_half = true;
		ringbuffer_write(&ring_buffer, one_byte);
		tasklet_schedule(&circular_tasklet);
		one_byte = 0;
	}

	return IRQ_HANDLED;
}

int __init gpio_dummy_init(void)
{
	int ret;

	gpio_dummy_base = (u32)ioremap(BCM2835_PERI_BASE + 0x200000, 4096);
	printk(KERN_WARNING "The gpio base is mapped to %x\n", gpio_dummy_base);
	ret = gpio_request_array(gpio_dummy, ARRAY_SIZE(gpio_dummy));

	if (ret) {
		printk(KERN_ERR
		       "Unable to request GPIOs for the dummy device: %d\n",
		       ret);
		return ret;
	}
	ret = gpio_to_irq(gpio_dummy[ARRAY_SIZE(gpio_dummy) - 1].gpio);
	if (ret < 0) {
		printk(KERN_ERR "Unable to request IRQ for gpio %d: %d\n",
		       gpio_dummy[ARRAY_SIZE(gpio_dummy) - 1].gpio, ret);
		goto fail1;
	}
	dummy_irq = ret;
	printk(KERN_WARNING "Successfully requested IRQ# %d for %s\n",
	       dummy_irq, gpio_dummy[ARRAY_SIZE(gpio_dummy) - 1].label);

	ret = request_irq(dummy_irq, dummyport_interrupt,
			  IRQF_TRIGGER_RISING | IRQF_ONESHOT, "gpio27", NULL);

	pr_warn("ret = %d", ret);

	if (ret) {
		printk(KERN_ERR "Unable to request IRQ for dummy device: %d\n",
		       ret);
		goto fail1;
	}

	//Allocate a major number for the device
	if (((ret = alloc_chrdev_region(&asgn2_device.dev, 0, 1, MYDEV_NAME)) <
	     0)) {
		pr_warn("%s: can't create chrdev_region\n", MYDEV_NAME);
		return ret;
	}
	// Create device class
	pr_info("created chrdev_region\n");
	if (IS_ERR(asgn2_device.class = class_create(MYDEV_NAME))) {
		pr_warn("%s: can't create class\n", MYDEV_NAME);
		ret = (int)PTR_ERR(asgn2_device.class);
		goto fail_class_create;
	}

	pr_info("set up class\n");

	// Create device
	if (IS_ERR(asgn2_device.device = device_create(asgn2_device.class, NULL,
						       asgn2_device.dev, "%s",
						       MYDEV_NAME))) {
		pr_warn("%s: can't create device\n", MYDEV_NAME);
		ret = (int)PTR_ERR(asgn2_device.device);
		goto fail_device_create;
	}

	pr_info("set up device entry\n");

	// Initialise the cdev and add it
	cdev_init(&asgn2_device.cdev, &asgn2_fops);
	if ((ret = cdev_add(&asgn2_device.cdev, asgn2_device.dev, 1)) < 0) {
		pr_warn("%s: can't create udev device\n", MYDEV_NAME);
		goto fail_cdev_add;
	}

	pr_info("set up udev entry\n");

	// Create a memory cache for page nodes
	if (IS_ERR(asgn2_device.cache =
			   kmem_cache_create("cache", sizeof(page_node), 0,
					     SLAB_HWCACHE_ALIGN, NULL))) {
		pr_err("kmem_cache_create failed\n");
		ret = (int)PTR_ERR(asgn2_device.cache);
		goto fail_kmem_cache_create;
	}
	pr_info("successfully created cache");

	// Create a proc entry
	if (IS_ERR(entry = proc_create(MY_PROC_NAME, 0660, NULL,
				       &asgn2_proc_ops))) {
		pr_err("Failed to create proc entry\n");
		ret = (int)PTR_ERR(entry);
		goto fail_proc_create;
	}
	pr_info("Successfully created proc entry");

	/* Initialise fields */
	INIT_LIST_HEAD(&asgn2_device.dlist.head);
	asgn2_device.num_pages = 0;
	atomic_set(&asgn2_device.dlist.count, 0);
	atomic_set(&asgn2_device.nprocs, 0);
	atomic_set(&asgn2_device.max_nprocs, 1);
	mutex_init(&asgn2_device.device_mutex);
	init_waitqueue_head(&asgn2_device.ptr_overlap_queue);
	atomic_set(&asgn2_device.file_count, 0);
	return 0;

	// Cleanup on error occuring
fail_proc_create:
	kmem_cache_destroy(asgn2_device.cache);
fail_kmem_cache_create:
	cdev_del(&asgn2_device.cdev);
fail_cdev_add:
	device_destroy(asgn2_device.class, asgn2_device.dev);
fail_device_create:
	class_destroy(asgn2_device.class);
fail_class_create:
	unregister_chrdev_region(asgn2_device.dev, 1);
	write_to_gpio(15);
fail1:
	gpio_free_array(gpio_dummy, ARRAY_SIZE(gpio_dummy));
	iounmap((void *)gpio_dummy_base);
	return ret;
}

void __exit gpio_dummy_exit(void)
{
	free_irq(dummy_irq, NULL);
	gpio_free_array(gpio_dummy, ARRAY_SIZE(gpio_dummy));
	iounmap((void *)gpio_dummy_base);
	free_memory_pages();
	kmem_cache_destroy(asgn2_device.cache);
	remove_proc_entry(MY_PROC_NAME, NULL);
	cdev_del(&asgn2_device.cdev);
	device_destroy(asgn2_device.class, asgn2_device.dev);
	class_destroy(asgn2_device.class);
	unregister_chrdev_region(asgn2_device.dev, 1);
	mutex_destroy(&asgn2_device.device_mutex);
}

module_init(gpio_dummy_init);
module_exit(gpio_dummy_exit);
