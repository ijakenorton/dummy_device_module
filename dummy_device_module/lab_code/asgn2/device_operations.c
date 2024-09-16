
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
	struct kmem_cache *cache; /* cache memory */
	struct class *class; /* the udev class */
	struct device *device; /* the udev device node */
} gpio_dev;

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
	/* Increment process count, if exceeds max_nprocs, return -EBUSY */
	int nprocs = atomic_read(&asgn2_device.nprocs) + 1;
	if (nprocs > atomic_read(&asgn2_device.max_nprocs)) {
		return -EBUSY;
	}

	atomic_set(&asgn2_device.nprocs, nprocs);
	/* if opened in write-only mode, free all memory pages */

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

ssize_t asgn2_read(struct file *filp, char __user *buf, size_t count,
		   loff_t *f_pos)
{
	size_t size_read = 0;
	int result = 0;
	page_node *curr;
	page_node *temp;
	char *kernel_buf;

	if (asgn2_device.read_offset + count > asgn2_device.data_size)
		count = asgn2_device.data_size - asgn2_device.read_offset;

	kernel_buf = kmalloc(count, GFP_KERNEL);
	if (!kernel_buf) {
		return -ENOMEM;
	}

	list_for_each_entry_safe(curr, temp, &asgn2_device.mem_list, list) {
		char *page_data = (char *)kmap(curr->page);
		if (!page_data) {
			result = -ENOMEM;
			goto out;
		}
		while (size_read < count) {
			if (curr->read_offset >= PAGE_SIZE) {
				kunmap(curr->page);

				if (curr->page) {
					__free_page(curr->page);
					curr->page = NULL;
				}

				list_del(&curr->list);
				kmem_cache_free(asgn2_device.cache, curr);
				goto end;
			}
			kernel_buf[size_read] = page_data[curr->read_offset];

			if (kernel_buf[size_read] == '\0') {
				kunmap(curr->page);
				goto out; // Found null byte, end of file
			}

			size_read++;
			asgn2_device.read_offset++;
			curr->read_offset++;

			if (size_read >= count)
				goto out;
		}
		kunmap(curr->page);
end:
		continue;
	}

out:
	if (size_read > 0) {
		if (copy_to_user(buf, kernel_buf, size_read) != 0) {
			result = -EFAULT;
		}
	}

	kfree(kernel_buf);
	asgn2_device.data_size -= size_read;
	asgn2_device.read_offset -= size_read;
	return result < 0 ? result : size_read;
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
