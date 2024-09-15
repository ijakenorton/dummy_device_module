
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
	int data_size;
} page_node;

typedef struct gpio_dev_t {
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
