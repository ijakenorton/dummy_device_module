
#include "linux/atomic/atomic-instrumented.h"
#include "linux/highmem-internal.h"
#include "ring_buffer.h"
#define BUF_SIZE 2000
#define MYDEV_NAME "asgn2"
#define MY_PROC_NAME "asgn2_proc"
#define MYIOC_TYPE 'k'
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

#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 3, 0)
#include <asm/switch_to.h>
#else
#include <asm/system.h>
#endif

typedef struct page_node_rec {
	struct list_head list;
	struct page *page;
	unsigned long write_offset;
	unsigned long read_offset;
} page_node;

typedef struct gpio_dev_t {
	dev_t dev; /* the device */
	struct cdev cdev;
	struct list_head mem_list;
	int num_pages; /* number of memory pages this module currently holds */
	size_t data_size; /* total data size in this module */
	size_t read_offset;
	atomic_t nprocs; /* number of processes accessing this device */
	atomic_t max_nprocs; /* max number of processes accessing this device */
	atomic_t data_ready;
	wait_queue_head_t data_queue;
	struct mutex device_mutex;
	struct kmem_cache *cache; /* cache memory */
	struct class *class; /* the udev class */
	struct device *device; /* the udev device node */
} gpio_dev;

#define print_fpos(val) pr_info(#val " = %lld", *val)
int asgn2_major = 0; /* major number of module */
int asgn2_minor = 0; /* minor number of module */
int asgn2_dev_count = 1; /* number of devices */
struct proc_dir_entry *entry;
gpio_dev asgn2_device;

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
int asgn2_open(struct inode *inode, struct file *filp)
{
	int ret;

	// Wait until the device is available
	ret = mutex_lock_interruptible(&asgn2_device.device_mutex);
	if (ret)
		return ret; // Return -ERESTARTSYS if interrupted

	// Check if it's opened for reading (read-only device)
	if ((filp->f_flags & O_ACCMODE) != O_RDONLY) {
		mutex_unlock(&asgn2_device.device_mutex);
		return -EACCES; // Return "Permission denied" for non-read-only access
	}

	// Increment process count (should always be 1 at this point)
	atomic_inc(&asgn2_device.nprocs);

	return 0; /* success */
}

int asgn2_release(struct inode *inode, struct file *filp)
{
	// Decrement process count
	atomic_dec(&asgn2_device.nprocs);

	// Release the mutex to allow other processes to open the device
	mutex_unlock(&asgn2_device.device_mutex);

	return 0;
}

ssize_t asgn2_read(struct file *filp, char __user *buf, size_t count,
		   loff_t *f_pos)
{
	size_t size_to_read = 0;
	size_t available_bytes = 0;
	int rv = 0;
	page_node *curr, *temp;
	char *kernel_buf;
	size_t size_written = 0;

	/* 	// Wait for data if the buffer is empty */
	/* 	while (asgn2_device.data_size == 0) { */
	/* 		if (filp->f_flags & O_NONBLOCK) */
	/* 			return -EAGAIN; */

	/* 		if (wait_event_interruptible(asgn2_device.data_queue, */
	/* 					     asgn2_device.data_size > 0)) */
	/* 			return -ERESTARTSYS; */
	/* 	} */
	/* if (asgn2_device.read_offset + *f_pos >= asgn2_device.data_size) { */
	/* 	pr_warn("asgn2_device.read_offset + *f_pos >= asgn2_device.data_size"); */
	/* 	return 0; */
	/* } */
	//find first EOF
	size_t num_loops = 0;
	list_for_each_entry(curr, &asgn2_device.mem_list, list) {
		char *page_data = kmap_local_page(curr->page);
		if (!page_data) {
			pr_warn("No page data");
			return -ENOMEM;
		}
		for (size_t i = curr->read_offset; i < PAGE_SIZE; i++) {
			if (available_bytes == asgn2_device.data_size) {
				pr_warn("Hit data size condition");
				print_zu(available_bytes);
				print_lu(curr->read_offset);
				kunmap_local(page_data);
				num_loops++;
				goto end_eof_loop;
			}
			if (page_data[i] == '\xFF') {
				pr_warn("found eof");
				print_zu(available_bytes);
				print_lu(curr->read_offset);
				pr_warn("end eof");
				kunmap_local(page_data);

				num_loops++;
				goto end_eof_loop;
			}

			available_bytes++;
		}
		num_loops++;
	}

end_eof_loop:
	if (available_bytes == 0) {
		curr->read_offset++;
		asgn2_device.read_offset++;
		/* curr->read_offset++; */
		/* asgn2_device.read_offset++; */
		pr_warn("Available bytes is 0");
		print_zu(asgn2_device.read_offset);
		print_lu(curr->read_offset);
		pr_warn("end available is  0");
		return 0;
	}
	size_to_read = min(available_bytes, count);
	kernel_buf = kmalloc(size_to_read, GFP_KERNEL);

	list_for_each_entry_safe(curr, temp, &asgn2_device.mem_list, list) {
		char *page_data = kmap_local_page(curr->page);
		if (!page_data) {
			pr_warn("No page data");
			return -ENOMEM;
		}
		for (size_t i = curr->read_offset; i < PAGE_SIZE; i++) {
			//add to kernel buffer for writing
			if (size_written < size_to_read) {
				kernel_buf[size_written++] = page_data[i];
			} else if (i >= available_bytes) {
				pr_warn("i >= available_bytes");
				print_lu(curr->read_offset);
				kunmap_local(page_data);
				goto end_write_loop;
			}
			curr->read_offset++;
		}

		kunmap_local(page_data);
	}

end_write_loop:
	if (size_written != 0) {
		size_t size_not_read =
			copy_to_user(buf, kernel_buf, size_written);
		if (size_not_read != 0) {
			if (size_written > 0) {
				rv = size_written - size_not_read;
				goto end;
			}

			rv = -EINVAL;
		}
		rv = size_written;
	}

end:

	//cleanup pages that have been written
	/* list_for_each_entry_safe(curr, temp, &asgn2_device.mem_list, list) { */
	/* 	if (curr->page) { */
	/* 		__free_page(curr->page); */
	/* 		curr->page = NULL; */
	/* 	} */

	/* 	list_del(&curr->list); */
	/* 	kmem_cache_free(asgn2_device.cache, curr); */

	/* 	asgn2_device.num_pages--; */

	/* 	num_loops--; */
	/* 	if (num_loops == 1) { */
	/* 		asgn2_device.data_size -= */
	/* 			(size_written - curr->read_offset); */
	/* 		break; */
	/* 	} */
	/* } */

	asgn2_device.data_size -= (size_written);
	asgn2_device.read_offset += available_bytes;
	*f_pos += available_bytes;

	kfree(kernel_buf);
	return rv;
}

/**
 * This function writes from the user buffer to the virtual disk of this
 * module
 */
ssize_t asgn2_write(struct file *filp, const char __user *buf, size_t count,
		    loff_t *f_pos)
{
	return -1;
}

#define SET_NPROC_OP 1
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
	}
	return -ENOTTY;
}
struct file_operations asgn2_fops = { .owner = THIS_MODULE,
				      .read = asgn2_read,
				      .write = asgn2_write,
				      .unlocked_ioctl = asgn2_ioctl,
				      .open = asgn2_open,
				      .release = asgn2_release };

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
	seq_printf(
		s,
		"Pages: %i\nMemory: %i bytes\nRead offset %zu\nDevices: %i\n",
		asgn2_device.num_pages, asgn2_device.data_size,
		asgn2_device.read_offset, atomic_read(&asgn2_device.nprocs));
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
