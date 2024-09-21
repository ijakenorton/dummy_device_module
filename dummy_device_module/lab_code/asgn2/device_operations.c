
#include "asm-generic/errno-base.h"
#define BUF_SIZE 2000
#include "ring_buffer.h"
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
#include <linux/list.h>

#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 3, 0)
#include <asm/switch_to.h>
#else
#include <asm/system.h>
#endif

#define EMPTY (1)
#define POINTER_OVERLAP (2)
#define MY_EOF (3)
#define SENTINEL '\0'
char read_buf[PAGE_SIZE] = { 0 };
typedef struct page_node_rec {
	struct list_head list;
	struct page *page;
	char *write_mapped;
	char *read_mapped;
	char *base_address;
	int position;
} page_node;

typedef struct {
	struct list_head head;
	page_node *read;
	page_node *write;
	atomic_t count;
} d_list_t;

typedef struct gpio_dev_t {
	dev_t dev; /* the device */
	struct cdev cdev;
	d_list_t dlist;
	size_t num_pages; /* number of memory pages this module currently holds */
	size_t data_size; /* total data size in this module */
	size_t read_offset;
	atomic_t nprocs; /* number of processes accessing this device */
	atomic_t max_nprocs; /* max number of processes accessing this device */
	atomic_t file_count;
	wait_queue_head_t ptr_overlap_queue;
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
page_node *allocate_node(void)
{
	page_node *new_node = kmem_cache_alloc(asgn2_device.cache, GFP_KERNEL);
	if (!new_node) {
		pr_err("Failed to allocate cache memory");
		return NULL;
	}
	new_node->page = alloc_page(GFP_KERNEL);
	if (!new_node->page) {
		kmem_cache_free(asgn2_device.cache, new_node);
		pr_err("Failed to allocate memory");
		return NULL;
	}

	new_node->base_address = (char *)kmap_local_page(new_node->page);
	new_node->write_mapped = new_node->base_address;
	new_node->read_mapped = new_node->base_address;
	new_node->position = -1;
	asgn2_device.num_pages++;
	return new_node;
}

void update_node_positions(d_list_t *dlist)
{
	page_node *node;
	int position = 0;

	list_for_each_entry(node, &dlist->head, list) {
		node->position = position++;
	}
}
// Utility function for looking at the current state of the dynamic list
void print_list_structure(d_list_t *dlist)
{
	struct list_head *pos;
	page_node *node;

	pr_info("Current read node position: %d\n", dlist->read->position);
	pr_info("Current write node position: %d\n", dlist->write->position);
	pr_info("List structure: count=%zu, max_size=%lu\n",
		atomic_read(&dlist->count), asgn2_device.num_pages * PAGE_SIZE);

	list_for_each(pos, &dlist->head) {
		node = list_entry(pos, page_node, list);
		pr_info("Node at position %d: base=%p, read=%p, write=%p\n",
			node->position, node->base_address, node->read_mapped,
			node->write_mapped);
	}
}
page_node *add_page_node(void)
{
	page_node *new_node = allocate_node();
	if (!new_node) {
		pr_err("Failed to allocate cache memory");
		return NULL;
	}
	list_add_tail(&new_node->list, &asgn2_device.dlist.head);
	update_node_positions(&asgn2_device.dlist);
	return new_node;
}

void free_memory_pages(void)
{
	page_node *curr;
	page_node *tmp;

	list_for_each_entry_safe(curr, tmp, &asgn2_device.dlist.head, list) {
		if (curr->page) {
			kunmap_local(curr->base_address);
			__free_page(curr->page);
			curr->page = NULL;
		}

		list_del(&curr->list);
		kmem_cache_free(asgn2_device.cache, curr);
	}

	asgn2_device.data_size = 0;
	asgn2_device.num_pages = 0;
}

void d_list_write(d_list_t *dlist, char value)
{
	page_node *new_node;
	if (list_empty(&dlist->head)) {
		pr_warn("list is empty");
		new_node = allocate_node();

		if (!new_node) {
			pr_err("Failed to allocate node");
			return;
		}
		list_add_tail(&new_node->list, &dlist->head);

		dlist->write = new_node;
		dlist->read = new_node;
		goto write;
	}

	//Assumes we have at least one page
	if (dlist->write->write_mapped ==
		    dlist->write->base_address + (int)PAGE_SIZE &&
	    atomic_read(&dlist->count) !=
		    asgn2_device.num_pages * (int)PAGE_SIZE) {
		pr_warn("page is full");

		//move to next page node
		dlist->write = list_next_entry_circular(dlist->write,
							&dlist->head, list);

		//set write pointer to base_address of the page
		dlist->write->write_mapped = dlist->write->base_address;
		goto write;
	}

	//handle case where circular is full, add new node between next and previous node
	//
	if (atomic_read(&dlist->count) ==
	    asgn2_device.num_pages * (int)PAGE_SIZE) {
		pr_warn("list is full");
		new_node = allocate_node();
		if (!new_node) {
			pr_err("Failed to allocate node");
			return;
		}
		__list_add(&new_node->list, &dlist->write->list,
			   dlist->write->list.next);

		dlist->write = new_node;
	}

write:
	// ASSERT to catch errors if something went wrong with the state
	// Can possibly use atomic_read_inc()
	if (atomic_read(&dlist->count) >
	    asgn2_device.num_pages * (int)PAGE_SIZE) {
		pr_err("d_list_write: count exceeded max_size\n");
		return;
	}
	// current page is full, moving to next element and resetting its write pointer
	*dlist->write->write_mapped++ = value;
	/* print_zu(dlist->count); */
	if (atomic_fetch_inc(&dlist->count) == 0) {
		wake_up_interruptible(&asgn2_device.ptr_overlap_queue);
	}
}

int d_list_peek(d_list_t *dlist)
{
	//shouldnt happen, this should be handled before this point
	if (atomic_read(&dlist->count) == 0) {
		pr_info("Count is zero, cannot read\n");
		return -EMPTY;
	}
	if (dlist->read->read_mapped == dlist->write->write_mapped) {
		return -POINTER_OVERLAP;
	}

	// move to the next page if we are at the end of this one
	if (dlist->read->read_mapped ==
	    dlist->read->base_address + (int)PAGE_SIZE) {
		dlist->read = list_next_entry_circular(dlist->read,
						       &dlist->head, list);

		//set read pointer to base_address of the page
		dlist->read->read_mapped = dlist->read->base_address;
	}
	char value = *dlist->read->read_mapped;
	if (value == SENTINEL) {
		pr_info("Peeked and found EOF");

		return -MY_EOF;
	}

	return 0;
}

int d_list_read(d_list_t *dlist, char *value)
{
	//shouldnt happen, this should be handled before this point
	if (atomic_read(&dlist->count) == 0) {
		pr_info("Count is zero, cannot read\n");
		return -EMPTY;
	}

	// move to the next page if we are at the end of this one
	if (dlist->read->read_mapped ==
	    dlist->read->base_address + (int)PAGE_SIZE) {
		dlist->read = list_next_entry_circular(dlist->read,
						       &dlist->head, list);

		//set read pointer to base_address of the page
		dlist->read->read_mapped = dlist->read->base_address;
	}

	*value = *dlist->read->read_mapped;
	dlist->read->read_mapped++;
	atomic_dec(&dlist->count);

	return 0;
}

int asgn2_open(struct inode *inode, struct file *filp)
{
	int ret = mutex_lock_interruptible(&asgn2_device.device_mutex);
	if (ret)
		return ret; // Return -EINTR if interrupted
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
	size_t size_written = 0;
	const size_t check = 0;
	char value;
	int rv = 0;
	if (count == 0)
		return 0;

	smp_rmb();
loop_start:

	if (wait_event_interruptible(asgn2_device.ptr_overlap_queue,
				     atomic_read(&asgn2_device.dlist.count) >
					     0)) {
		return -EINTR;
	}

	smp_rmb();
	size_to_read =
		min((size_t)atomic_read(&asgn2_device.dlist.count), count);

	while (size_written < size_to_read && size_written < PAGE_SIZE) {
		switch (d_list_peek(&asgn2_device.dlist)) {
		case -EMPTY:
			goto loop_start;

		case -MY_EOF:
			pr_info("EOF");
			if (size_written == check) {
				atomic_dec(&asgn2_device.file_count);
				smp_mb(); // Full barrier after modifying atomic variable
				d_list_read(&asgn2_device.dlist, &value);
				return 0;
			}

			pr_warn("got to the end of file");
			goto end_write_loop;
			break;

		case -POINTER_OVERLAP:
			if (size_written != 0)
				goto end_write_loop;

			pr_warn("finished waiting for pointer overlap\n");
			goto loop_start;
		default:
			switch (d_list_read(&asgn2_device.dlist, &value)) {
			case -EMPTY:
				goto loop_start;
			}

			smp_mb(); // Full barrier after reading from d_list
			asgn2_device.data_size--;
		}

		*(read_buf + size_written) = value;
		size_written++;
		*f_pos += 1;
	}

end_write_loop:
	smp_wmb();
	if (size_written != 0) {
		size_t size_not_read =
			copy_to_user(buf, read_buf, size_written);
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

	return rv;
}

/**
 * This function writes from the user buffer to the virtual disk of this
 * module
 */
ssize_t asgn2_write(struct file *filp, const char __user *buf, size_t count,
		    loff_t *f_pos)
{
	return -EACCES; // Return "Permission denied" for non-read-only access
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
	/*  set max_nprocs accordingly, */
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
	//Now that count is atomic might not be ideal to poll this...
	seq_printf(
		s,
		"Pages: %d\nMemory: %zu bytes\nCount: %zu\nRead offset %d\nDevices: %d\n",
		asgn2_device.num_pages, asgn2_device.data_size,
		atomic_read(&asgn2_device.dlist.count),
		atomic_read(&asgn2_device.file_count),
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
