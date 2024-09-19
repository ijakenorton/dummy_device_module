/**
 * File: asgn2.c
 * Date: 08/08/2024
 * Author: Jake Norton
 * Version: 0.1
 *
 * This is a module which serves as a virtual ramdisk which disk size is
 * limited by the amount of memory available and serves as the requirement for
 * COSC440 assignment 1. This template is provided to students for their 
 * convenience and served as hints/tips, but not necessarily as a standard
 * answer for the assignment. So students are free to change any part of
 * the template to fit their design, not the other way around. 
 *
 * Note: multiple devices and concurrent modules are not supported in this
 *       version. The template is 
 */

/* This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/gpio.h>
#include <linux/interrupt.h>
#include "asm/page.h"
#include "linux/atomic/atomic-instrumented.h"
#include "linux/device/class.h"
#include "linux/err.h"
#include "linux/gfp.h"
#include "linux/kern_levels.h"
#include "linux/printk.h"
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/list.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/device.h>
#include <linux/sched.h>

#define MYDEV_NAME "asgn2"
#define MY_PROC_NAME "asgn2_proc"
#define MYIOC_TYPE 'k'
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jake Norton");
MODULE_DESCRIPTION("COSC440 asgn2");

/**
 * The node structure for the memory page linked list.
 */
typedef struct page_node_rec {
	struct list_head list;
	struct page *page;
} page_node;

typedef struct asgn2_dev_t {
	dev_t dev; /* the device */
	struct cdev cdev;
	struct list_head mem_list;
	int num_pages; /* number of memory pages this module currently holds */
	size_t data_size; /* total data size in this module */
	atomic_t nprocs; /* number of processes accessing this device */
	atomic_t max_nprocs; /* max number of processes accessing this device */
	struct kmem_cache *cache; /* cache memory */
	struct class *class; /* the udev class */
	struct device *device; /* the udev device node */
} asgn2_dev;

struct proc_dir_entry *entry;
asgn2_dev asgn2_device;

int asgn2_major = 0; /* major number of module */
int asgn2_minor = 0; /* minor number of module */
int asgn2_dev_count = 1; /* number of devices */

/* static void data_transfer(unsigned long t_arg) */
/* { */
/* 	struct simp *datum; */
/* 	datum = (struct simp *)t_arg; */
/* 	printk(KERN_INFO "I am in t_fun, jiffies = %ld\n", jiffies); */
/* 	printk(KERN_INFO " I think my current task pid is %d\n", */
/* 	       (int)current->pid); */
/* 	printk(KERN_INFO " my data is: %d\n", datum->len); */
/* } */

/* static DECLARE_TASKLET_OLD(device_tasklet, data_transfer); */
/**
 * This function frees all memory pages held by the module.
 */

irqreturn_t dummyport_interrupt(int irq, void *dev_id)
{
	pr_info("recieved interupt");
	return IRQ_NONE;
}
void free_memory_pages(void)
{
	page_node *curr;
	page_node *tmp;
	list_for_each_entry_safe(curr, tmp, &asgn2_device.mem_list, list) {
		if (curr->page) {
			__free_page(curr->page);
			curr->page = NULL;
		}

		list_del(&curr->list);
		kmem_cache_free(asgn2_device.cache, curr);
	}

	asgn2_device.data_size = 0;
	asgn2_device.num_pages = 0;
}

/**
 * This function opens the virtual disk, if it is opened in the write-only
 * mode, all memory pages will be freed.
 */
int asgn2_open(struct inode *inode, struct file *filp)
{
	/* Increment process count, if exceeds max_nprocs, return -EBUSY */
	int nprocs = atomic_read(&asgn2_device.nprocs) + 1;
	if (nprocs > atomic_read(&asgn2_device.max_nprocs)) {
		return -EBUSY;
	}

	atomic_set(&asgn2_device.nprocs, nprocs);
	/* if opened in write-only mode, free all memory pages */
	if ((filp->f_flags & O_ACCMODE) == O_WRONLY) {
		pr_info("Device opened in write-only mode\n");
		free_memory_pages();
	}

	return 0; /* success */
}

/**
 * This function releases the virtual disk, but nothing needs to be done
 * in this case. 
 */
int asgn2_release(struct inode *inode, struct file *filp)
{
	int nprocs = atomic_read(&asgn2_device.nprocs);
	if (nprocs > 0) {
		atomic_set(&asgn2_device.nprocs, nprocs - 1);
	}
	return 0;
}

/**
 * This function reads contents of the virtual disk and writes to the user 
 */
ssize_t asgn2_read(struct file *filp, char __user *buf, size_t count,
		   loff_t *f_pos)
{
	if (*f_pos >= asgn2_device.data_size) {
		return 0;
	}

	size_t size_read = 0;
	size_t begin_offset = (size_t)*f_pos % PAGE_SIZE;
	int begin_page_no = *f_pos / PAGE_SIZE;
	count = min(count, asgn2_device.data_size - (size_t)*f_pos);

	int curr_page_no;
	size_t size_to_be_read;

	if (asgn2_device.mem_list.next == NULL) {
		pr_warn("The asgn2_device.mem_list.next pointer is null");
		return -EFAULT;
	}
	struct list_head *ptr = asgn2_device.mem_list.next;
	page_node *curr;

	for (curr_page_no = 0; curr_page_no < begin_page_no; curr_page_no++) {
		ptr = ptr->next;
	}

	curr = list_entry(ptr, page_node, list);
	unsigned long size_not_read;

	while (size_read < count) {
		if (!curr) {
			return size_read;
		}
		if (!curr->page) {
			return size_read;
		}
		size_to_be_read = min((count - size_read),
				      PAGE_SIZE - (size_t)begin_offset);

		void *current_address = page_address(curr->page);
		current_address += begin_offset;
		size_not_read =
			copy_to_user(buf, current_address, size_to_be_read);

		if (size_not_read != 0) {
			if (size_read > 0) {
				pr_warn("size_not_read %lu exiting...",
					size_not_read);
				return size_not_read;
			}
			return -EINVAL;
		}
		size_read += size_to_be_read;
		*f_pos += size_to_be_read;
		buf += size_to_be_read;
		begin_offset = 0;
		if (size_read < count) {
			if (list_is_last(&curr->list, &asgn2_device.mem_list)) {
				pr_info("size_not_read %lu exiting...",
					size_not_read);
				pr_info("list is last size_read %zu exiting...",
					size_read);
				return size_read;
			}
			curr = list_entry(curr->list.next, page_node, list);
		}
	}

	pr_info("Finished read successfully after reading %d", size_read);

	return size_read;
}

static loff_t asgn2_lseek(struct file *file, loff_t offset, int cmd)
{
	loff_t test_pos = 0;
	size_t buffer_size = asgn2_device.num_pages * PAGE_SIZE;

	switch (cmd) {
	case SEEK_SET:
		test_pos = offset;
		break;
	case SEEK_CUR:
		test_pos = file->f_pos + offset;
		break;
	case SEEK_END:
		test_pos = buffer_size + offset;
		break;
	default:
		return -EINVAL;
	}
	if (test_pos < 0) {
		return -EINVAL;
	}

	if (test_pos > buffer_size) {
		test_pos = buffer_size;
	}

	file->f_pos = test_pos;

	return test_pos;
}

/**
 * This function writes from the user buffer to the virtual disk of this
 * module
 */
ssize_t asgn2_write(struct file *filp, const char __user *buf, size_t count,
		    loff_t *f_pos)
{
	if (count <= 0) {
		return 0;
	}
	/* Shouldn't be possible*/
	if (*f_pos > asgn2_device.data_size) {
		pr_warn("Starting f_pos %lld is more than data_size, stopping write..",
			*f_pos);
		return -EINVAL;
	}
	size_t orig_f_pos = *f_pos;
	size_t size_written = 0;
	size_t begin_offset = *f_pos % PAGE_SIZE;
	int begin_page_no = *f_pos / PAGE_SIZE;
	int curr_page_no;
	size_t size_to_be_written;

	size_t size_not_written;
	page_node *curr;

	//Traverse the list until the first page reached, and add nodes if necessary

	if (list_empty(&asgn2_device.mem_list)) {
		curr = kmem_cache_alloc(asgn2_device.cache, GFP_KERNEL);
		if (curr == NULL) {
			pr_err("Failed to allocate cache memory");
			return -ENOMEM;
		}

		curr->page = alloc_page(GFP_KERNEL);
		if (!curr->page) {
			kmem_cache_free(asgn2_device.cache, curr);
			pr_err("Failed to allocate memory");
			return -ENOMEM;
		}
		list_add_tail(&curr->list, &asgn2_device.mem_list);
		asgn2_device.num_pages++;
	} else {
		// find the starting page
		curr = list_first_entry(&asgn2_device.mem_list, page_node,
					list);
		for (curr_page_no = 0; curr_page_no < begin_page_no;
		     curr_page_no++) {
			if (list_is_last(&curr->list, &asgn2_device.mem_list)) {
				break;
			}
			curr = list_entry(curr->list.next, page_node, list);
		}
	}
	/* Then write the data page by page*/

	while (size_written < count) {
		// There is still size to be written
		// We are not at the start of the device
		// We need to start writing at the start of next page
		if (*f_pos != 0 && begin_offset == 0) {
			if (list_is_last(&curr->list, &asgn2_device.mem_list)) {
				curr = kmem_cache_alloc(asgn2_device.cache,
							GFP_KERNEL);

				if (!curr) {
					pr_err("Failed to allocate cache memory");
					return size_written > 0 ? size_written :
								  -ENOMEM;
				}
				curr->page = alloc_page(GFP_KERNEL);

				if (!curr->page) {
					kmem_cache_free(asgn2_device.cache,
							curr);
					pr_err("Failed to allocate memory");
					return size_written > 0 ? size_written :
								  -ENOMEM;
				}
				list_add_tail(&curr->list,
					      &asgn2_device.mem_list);
				asgn2_device.num_pages++;
			} else {
				curr = list_entry(curr->list.next, page_node,
						  list);
			}
		}
		size_to_be_written =
			min((count - size_written), PAGE_SIZE - begin_offset);
		size_not_written =
			copy_from_user(page_address(curr->page) + begin_offset,
				       buf, size_to_be_written);
		if (size_not_written != 0) {
			pr_warn("size_not_written %zu exiting...",
				size_not_written);
			return size_written > 0 ?
				       size_written - size_not_written :
				       -EFAULT;
		}
		size_written += size_to_be_written;
		*f_pos += size_to_be_written;
		buf += size_to_be_written;
		begin_offset = 0;
	}

	asgn2_device.data_size =
		max(asgn2_device.data_size, orig_f_pos + size_written);
	return size_written;
}

#define SET_NPROC_OP 1
#define SET_MMAP_OP 2
#define MMAP_DEV_CMD_GET_BUFSIZE _IOW(MYIOC_TYPE, SET_MMAP_OP, int)
#define TEM_SET_NPROC _IOW(MYIOC_TYPE, SET_NPROC_OP, int)

long asgn2_ioctl(struct file *filp, unsigned cmd, unsigned long arg)
{
	int new_max_nprocs;

	/* check whether cmd is for our device, if not for us, return -EINVAL */
	if (_IOC_TYPE(cmd) != MYIOC_TYPE)
		return -EINVAL;

	/*  get command, and if command is TEM_SET_NPROC, then get the data, and */
	/*  set max_nprocs accordingly,
         *  if MMAP_DEV_CMD_GET_BUFSIZE set user value to the current datasize*/
	switch (cmd) {
	case TEM_SET_NPROC:
		if (!access_ok((void __user *)arg,
			       sizeof(asgn2_device.max_nprocs)))
			return -EINVAL;

		if (get_user(new_max_nprocs, (int __user *)arg)) {
			return -EFAULT;
		}
		atomic_set(&asgn2_device.max_nprocs, new_max_nprocs);
		return 0;
	case MMAP_DEV_CMD_GET_BUFSIZE:
		if (!access_ok((void __user *)arg,
			       sizeof(asgn2_device.data_size)))
			return -EINVAL;
		if (put_user(asgn2_device.data_size, (size_t __user *)arg)) {
			return -EFAULT;
		}
		return 0;
	}
	return -ENOTTY;
}

static int asgn2_mmap(struct file *filp, struct vm_area_struct *vma)
{
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long len = vma->vm_end - vma->vm_start;
	unsigned long ramdisk_size = asgn2_device.num_pages * PAGE_SIZE;
	unsigned long index;
	unsigned long begin_page_no = offset / PAGE_SIZE;
	unsigned long pages_to_map = (len + PAGE_SIZE - 1) / PAGE_SIZE;
	page_node *curr;
	/* check offset and len */
	if (offset + len > ramdisk_size) {
		pr_warn("Offset + length are out of bounds\n");
		return -EINVAL;
	}
	struct list_head *ptr = asgn2_device.mem_list.next;

	/* loop through the entire page list, once the first requested page */
	for (index = 0; index < begin_page_no; index++) {
		ptr = ptr->next;
	}

	curr = list_entry(ptr, page_node, list);
	/* add each page with remap_pfn_range one by one */
	/* up to the last requested page */
	for (index = 0; index < pages_to_map; index++) {
		if (remap_pfn_range(vma, vma->vm_start + index * PAGE_SIZE,
				    page_to_pfn(curr->page), PAGE_SIZE,
				    vma->vm_page_prot)) {
			pr_warn("Failed to map page at index %lu\n", index);
			return -EAGAIN;
		}

		curr = list_entry(curr->list.next, page_node, list);
	}

	return 0;
}

struct file_operations asgn2_fops = { .owner = THIS_MODULE,
				      .read = asgn2_read,
				      .write = asgn2_write,
				      .unlocked_ioctl = asgn2_ioctl,
				      .open = asgn2_open,
				      .mmap = asgn2_mmap,
				      .release = asgn2_release,
				      .llseek = asgn2_lseek };

static void *my_seq_start(struct seq_file *s, loff_t *pos)
{
	if (*pos >= 1)
		return NULL;
	else
		return &asgn2_dev_count + *pos;
}
static void *my_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	(*pos)++;
	if (*pos >= 1)
		return NULL;
	else
		return &asgn2_dev_count + *pos;
}
static void my_seq_stop(struct seq_file *s, void *v)
{
	/* There's nothing to do here! */
}

int my_seq_show(struct seq_file *s, void *v)
{
	seq_printf(s, "Pages: %i\nMemory: %i bytes\nDevices: %i\n",
		   asgn2_device.num_pages, asgn2_device.data_size,
		   atomic_read(&asgn2_device.nprocs));
	return 0;
}

static struct seq_operations my_seq_ops = { .start = my_seq_start,
					    .next = my_seq_next,
					    .stop = my_seq_stop,
					    .show = my_seq_show };

static int my_proc_open(struct inode *inode, struct file *filp)
{
	return seq_open(filp, &my_seq_ops);
}

static struct proc_ops asgn2_proc_ops = {
	.proc_open = my_proc_open,
	.proc_lseek = seq_lseek,
	.proc_read = seq_read,
	.proc_release = seq_release,
};

/**
 * Initialise the module and create the master device
 */
int __init asgn2_init_module(void)
{
	int rv;

	//Allocate a major number for the device
	if (((rv = alloc_chrdev_region(&asgn2_device.dev, 0, 1, MYDEV_NAME)) <
	     0)) {
		pr_warn("%s: can't create chrdev_region\n", MYDEV_NAME);
		return rv;
	}
	// Create device class
	pr_info("created chrdev_region\n");
	if (IS_ERR(asgn2_device.class = class_create(MYDEV_NAME))) {
		pr_warn("%s: can't create class\n", MYDEV_NAME);
		rv = (int)PTR_ERR(asgn2_device.class);
		goto fail_class_create;
	}

	pr_info("set up class\n");

	// Create device
	if (IS_ERR(asgn2_device.device = device_create(asgn2_device.class, NULL,
						       asgn2_device.dev, "%s",
						       MYDEV_NAME))) {
		pr_warn("%s: can't create device\n", MYDEV_NAME);
		rv = (int)PTR_ERR(asgn2_device.device);
		goto fail_device_create;
	}

	pr_info("set up device entry\n");

	// Initialise the cdev and add it
	cdev_init(&asgn2_device.cdev, &asgn2_fops);
	if ((rv = cdev_add(&asgn2_device.cdev, asgn2_device.dev, 1)) < 0) {
		pr_warn("%s: can't create udev device\n", MYDEV_NAME);
		goto fail_cdev_add;
	}

	pr_info("set up udev entry\n");

	// Create a memory cache for page nodes
	if (IS_ERR(asgn2_device.cache =
			   kmem_cache_create("cache", sizeof(page_node), 0,
					     SLAB_HWCACHE_ALIGN, NULL))) {
		pr_err("kmem_cache_create failed\n");
		rv = (int)PTR_ERR(asgn2_device.cache);
		goto fail_kmem_cache_create;
	}
	pr_info("successfully created cache");

	// Create a proc entry
	if (IS_ERR(entry = proc_create(MY_PROC_NAME, 0660, NULL,
				       &asgn2_proc_ops))) {
		pr_err("Failed to create proc entry\n");
		rv = (int)PTR_ERR(entry);
		goto fail_proc_create;
	}
	pr_info("Successfully created proc entry");

	/* Initialise fields */
	INIT_LIST_HEAD(&asgn2_device.mem_list);
	asgn2_device.num_pages = 0;
	atomic_set(&asgn2_device.nprocs, 0);
	atomic_set(&asgn2_device.max_nprocs, 1);
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

	return rv;
}

/**
 * Finalise the module
 */
void __exit asgn2_exit_module(void)
{
	free_memory_pages();
	kmem_cache_destroy(asgn2_device.cache);
	remove_proc_entry(MY_PROC_NAME, NULL);
	cdev_del(&asgn2_device.cdev);
	device_destroy(asgn2_device.class, asgn2_device.dev);
	class_destroy(asgn2_device.class);
	unregister_chrdev_region(asgn2_device.dev, 1);

	pr_info("Good bye from %s\n", MYDEV_NAME);
}

module_init(asgn2_init_module);
module_exit(asgn2_exit_module);
