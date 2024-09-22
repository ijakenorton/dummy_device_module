## Concurrency model

The only locks involved are within the atomic reads. However atomic reads do happen a lot, which
will reduce concurrency and performance, however it is necessary for maintaining the internal state
of the circular list. The current concurrency model does assume only one writer ( producer ) and one
reader ( consumer ) at a time, if this assumption is broken, the memory will end up in an
inconsistent state. As far as I have tested it did not result in a kernel panic, but it did result
in lost / mixed up data.

## Memory Management

Currently the list is only freed when the module is removed or the count is set to zero(this means
all the data has been read). This means there is potential for the memory to be larger than needed
if the reads never catch up to the writes. This could be solved by something like a mark and sweep
garbage collection, where page_nodes that have been read and marked and the garbage collector sweeps
either on an interval or after a reader releases. I have not tried to do this as it seems to require
an addition of spinlocks over a lot of operations, or some sort of copying / complex pointer moving
and saving. This is made more difficult due to the circular nature of the list.

I did try a simpler idea which frees up until the current page_node pointed to by the reader,
starting from the writer node as everything in-between is guaranteed to have been read. This can be
found in the `d_free_up_till_read()` function. Currently there is an issue though, perhaps I am going
one page too far. Either way I don't have time to implement this but in theory this cleanup could be
called based on some sort of metric. E.g. If the list is 50% empty, removed all unused nodes. This
should not happen all the time as it would defeat the purpose of having a circular list.

## Barriers

Currently there are many full or partial memory barriers in place to ensure that the cache has been
flushed before reads and writes to make sure the data reads are the up to date values. I am unsure
if all of these are actually needed, and the inclusion of them does add some overhead as the cache
cannot be abused for faster reads. I believe to removed these and be confident of robustness would
take lots of testing that I have not done as of yet.

The current implementation will wait indefinitely if there is no data in the device. The waiting is
interruptible though. I did wonder if it would be useful to have the wait be on a max timer, unsure
if that makes sense though.

## Page Limit

Currently the module is setup to have a `MAX_PAGE` which is a limit to which the memory list can grow.
This seems prudent, especially given that there is no guarantee of memory being freed until the
device / module is released. This is currently set to 100000 pages, this can be changed by changing
the `MAX_PAGE` size macro. When at `MAX_PAGE` the module will not write/add a new page. However if it
encounters a EOF value it will overwrite the previous value with that to attempt to keep the file
boundaries as much as possible.

## Read

The function called by read() from the user perspective, follows general read conventions. It does
however let the user read more than one file if they do not release the file handle after I have
returned 0. This is not ideal but I do not have time to fix it currently, it makes the handling of
when to increment the internal read pointer easier this way as I do not have to handle state
relative to the current process. The current read function uses a buffer of constant size(`read_buf`)
to collect the data before transferring it to the user. This allows for no dynamic memory
allocation, but does mean there are more calls of copy to user. The buffer is currently PAGE_SIZE in
length, but this can be changed by changing the `READ_BUFFER_SIZE` macro. This approach allows for
only one pass through the data, as we do not need to know the length of the current file ahead of
time in order to allocate the memory, which fit the single-character-look-ahead style of
data-structure I used. Currently this returns after the buffer is full. Ideally it would do multiple
copy_to_users in the same read call in order to reduce context switching. This is something I will
fix, but it is an inefficiency currently.
