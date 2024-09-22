#ifndef kernel_includes_H
#include "kernel_includes.h"
#endif /* ifndef kernel_includes_HS */
#define MYDEV_NAME "asgn2"
#define MY_PROC_NAME "asgn2_proc"
#define MYIOC_TYPE 'k'
#define MIN_PAGE (5)
#define MAX_PAGE (100000)
#define EMPTY (1)
#define POINTER_OVERLAP (2)
#define MY_EOF (3)
#define SENTINEL '\0'
#define READ_BUFFER_SIZE PAGE_SIZE
char read_buf[READ_BUFFER_SIZE] = { 0 };
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
	atomic_t unread_bytes;
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
}
gpio_dev;

#define print_fpos(val) pr_info(#val " = %lld", *val)
int asgn2_major = 0; /* major number of module */
int asgn2_minor = 0; /* minor number of module */
int asgn2_dev_count = 1; /* number of devices */
struct proc_dir_entry *entry;
gpio_dev asgn2_device;

// Allocates a number page node using the cache as the memory pool.
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

//Utility function to calculate node position field
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
		atomic_read(&dlist->unread_bytes),
		asgn2_device.num_pages * PAGE_SIZE);

	list_for_each(pos, &dlist->head) {
		node = list_entry(pos, page_node, list);
		pr_info("Node at position %d: base=%p, read=%p, write=%p\n",
			node->position, node->base_address, node->read_mapped,
			node->write_mapped);
	}
}
// Allocate and add a page node at the tail of the list
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

// Free all pages
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
// Cleanup function, something is not quite right with the logic, don't have time to fix right now
// so this is on the todo for a way to clean up memory
void d_free_pages_up_till_read(void)
{
	page_node *curr;
	page_node *temp;

	//Modified version of list_for_each_safe to work for circular lists and to break when hit
	//the current read node
	for (curr = list_next_entry_circular(asgn2_device.dlist.write,
					     &asgn2_device.dlist.head, list),
	    temp = list_next_entry_circular(curr, &asgn2_device.dlist.head,
					    list);
	     curr != asgn2_device.dlist.read; curr = temp,
	    temp = list_next_entry_circular(temp, &asgn2_device.dlist.head,
					    list)) {
		if (curr->page) {
			kunmap_local(curr->base_address);
			__free_page(curr->page);
			curr->page = NULL;
		}

		list_del(&curr->list);
		kmem_cache_free(asgn2_device.cache, curr);

		asgn2_device.num_pages--;
	}
}

// Basic cleanup function which resets the list down to one page if the list is empty(count == 0)
void d_free_pages_after_first(void)
{
	page_node *curr;
	page_node *tmp;

	list_for_each_entry_safe(curr, tmp, &asgn2_device.dlist.head, list) {
		if (list_is_first(&curr->list, &asgn2_device.dlist.head)) {
			continue;
		}
		if (curr->page) {
			kunmap_local(curr->base_address);
			__free_page(curr->page);
			curr->page = NULL;
		}

		list_del(&curr->list);
		kmem_cache_free(asgn2_device.cache, curr);
	}

	asgn2_device.data_size = 0;
	asgn2_device.num_pages = 1;
}

// Write function which takes into account the current state of the memory list. This can be treated
// as a thread safe iterator. It will not create new pages after MAX_PAGE. It also handles some of
// the cleanup as it makes the concurrency lockless. It does rely on there being only one reader and
// one writer at a time. If those assumptions are broken the list can end up in a consistent state.
void d_list_write(d_list_t *dlist, char value)
{
	page_node *node;
	//Initialize the list
	if (list_empty(&dlist->head)) {
		pr_warn("list is empty");
		node = allocate_node();

		if (!node) {
			pr_err("Failed to allocate node");
			return;
		}
		list_add_tail(&node->list, &dlist->head);

		dlist->write = node;
		dlist->read = node;
		goto write;
	}

	// Only cleanup if count is < than half of max size
	/* if (asgn2_device.num_pages > MIN_PAGE && */
	/*     atomic_read(&dlist->count) < */
	/* 	    ((asgn2_device.num_pages * (int)PAGE_SIZE) / 2)) { */
	/* 	pr_warn("Starting free after first GC"); */
	/* 	d_free_pages_up_till_read(); */
	/* } */

	if (asgn2_device.num_pages > MIN_PAGE &&
	    atomic_read(&dlist->unread_bytes) == 0) {
		pr_warn("Starting GC");
		print_zu(asgn2_device.num_pages);

		node = dlist->write = dlist->read =
			list_first_entry(&dlist->head, page_node, list);

		node->write_mapped = node->read_mapped = node->base_address;
		d_free_pages_after_first();
	}

	//Assumes we have at least one page
	if (dlist->write->write_mapped ==
		    dlist->write->base_address + (int)PAGE_SIZE &&
	    atomic_read(&dlist->unread_bytes) !=
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
	if (atomic_read(&dlist->unread_bytes) ==
	    asgn2_device.num_pages * (int)PAGE_SIZE) {
		if (asgn2_device.num_pages >= MAX_PAGE) {
			pr_warn("Pages over max page size (%d), not longer appending to list",
				MAX_PAGE);
			// Don't want to miss a EOF value, so replace what was there to at least
			// maintain file boundaries
			if (value == SENTINEL) {
				*dlist->write->write_mapped = value;
			}
			return;
		}
		node = allocate_node();
		if (!node) {
			pr_err("Failed to allocate node");
			return;
		}
		// Add node after current write node, this can be anywhere on the list depending on
		// how the read / write patterns on the device
		__list_add(&node->list, &dlist->write->list,
			   dlist->write->list.next);

		dlist->write = node;
	}

write:
	// ASSERT to catch errors if something went wrong with the state
	// Can possibly use atomic_read_inc()
	if (atomic_read(&dlist->unread_bytes) >
	    asgn2_device.num_pages * (int)PAGE_SIZE) {
		pr_err("d_list_write: count exceeded max_size\n");
		return;
	}
	// current page is full, moving to next element and resetting its write pointer
	*dlist->write->write_mapped++ = value;
	if (atomic_fetch_inc(&dlist->unread_bytes) == 0) {
		wake_up_interruptible(&asgn2_device.ptr_overlap_queue);
	}
}

// This is used to peek into to the list to check the current list condition. It returns -EMPTY if
// the list empty(unread_bytes == 0), -POINTER_OVERLAP if the read and write pointers point to the
// same place(same page and same position on that page). Returns -MY_EOF if a SENTINEL value is
// found. Returns 0 for a normal case where it finds an ascii value. This is thread safe assuming
// the one reader and one writer paradigm. This method handles traversing the circular list.
int d_list_peek(d_list_t *dlist)
{
	//shouldnt happen, this should be handled before this point
	if (atomic_read(&dlist->unread_bytes) == 0) {
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

// This function checks the current read position (dlist->read->read_mapped pointer).This is thread safe assuming
// the one reader and one writer paradigm. This function should be called after d_list_peek as the
// peek function checks the list conditions more thoroughly. This function still checks for if the
// list is empty or a page is full however these are in someways asserts, the peek function should
// already have handled such cases. Upon successful read, this will move the read->read_mapped
// pointer forward and decrement unread_bytes
int d_list_read(d_list_t *dlist, char *value)
{
	//shouldnt happen, this should be handled before this point
	if (atomic_read(&dlist->unread_bytes) == 0) {
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
	atomic_dec(&dlist->unread_bytes);

	return 0;
}

// Open allows only one process at a time, if more than one process tries to access the device it
// will wait until the previous device has released.
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

	return 0;
}
// Releases device mutex, decrements the number of processes(nprocs)
int asgn2_release(struct inode *inode, struct file *filp)
{
	// Decrement process count
	atomic_dec(&asgn2_device.nprocs);

	// Release the mutex to allow other processes to open the device
	mutex_unlock(&asgn2_device.device_mutex);

	return 0;
}

// The function called by read() from the user perspective, follows general read conventions. It
// does however let the user read more than one file if they do not release the file handle after I
// have returned 0.This is thread safe assuming the one reader and one writer paradigm.
// This method uses a buffer of constant size(read_buf) to collect the data before transferring it
// to the user. This allows for no dynamic memory allocation, but does mean there are more calls of
// copy to user. The buffer is currently PAGE_SIZE in length, but this can be changed by changing
// the READ_BUFFER_SIZE macro. This approach allows for only one pass through the data, as we do not
// need to know the length of the current file ahead of time in order to allocate the memory.
// Currently this returns after the buffer is full. Ideally it would do multiple copy_to_users in
// the same read call in order to reduce context switching. This is something I will fix, but it is
// an inefficiency currently.
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
	// This label is used as a reset if the list is in a non-normal status e.g -EMPTY or
	// -POINTER_OVERLAP. Instead of handling the cases there, the program reverts back to here
	// to have a consistent way of interacting with the internal list
loop_start:

	//The current reader is added to a wait queue if the number of unread_bytes is 0. This is
	//then woken up by the write to signal it to start reading again and there is data to be
	//read.
	if (wait_event_interruptible(
		    asgn2_device.ptr_overlap_queue,
		    atomic_read(&asgn2_device.dlist.unread_bytes) > 0)) {
		return -EINTR;
	}

	smp_rmb();
	size_to_read = min(
		(size_t)atomic_read(&asgn2_device.dlist.unread_bytes), count);

	// General process, peek first, then act accordingly, if hit an 'Error state' go back to
	// start of loop and try again, otherwise return if EOF or continue until an 'Error state'
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
 * This is a read-only device from the user point of view.
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
	// The number of processes can be changed, but this is just to show IOCTL potential. For now
	// the max_nprocs isn't really relevant as there can only be one anyway
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

// Can be polled to keep track of the current state of the list
// asgn2_device.data_size is a non_atomic version of unread_bytes, it also is a way to see how many
// total files have been read, as it does not decrement on file release
int my_seq_show(struct seq_file *s, void *v)
{
	//Now that count is atomic might not be ideal to poll this...
	seq_printf(
		s,
		"Pages: %d\nMemory: %zu bytes\nCount: %zu\nRead offset %d\nDevices: %d\n",
		asgn2_device.num_pages, asgn2_device.data_size,
		atomic_read(&asgn2_device.dlist.unread_bytes),
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
