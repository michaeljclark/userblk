# Userspace block device driver

This is a simple lightweight implementation of a kernel to userspace
block device driver interface.

Linux Kernel 2.6 has made it much more reliable to handle block device
requests in userspace due to the separation of writeback into the 'pdflush'
thread lessening the likelyhood of deadlocks (reentrancy in the userspace
deamon).

I've had very good results with this code under quite heavy memory
pressure (you still need a carefully written userspace which mlocks itself
into memory and avoids doing certain things).

I wrote it because the nbd and enbd implementations didn't provide a
nice and/or simple interface for a local userspace daemon.

enbd was closer to what I needed but when I looked at it I thought it's
userspace blocking on a ioctl was an ugly design - plus it was overcomplicated
with enbd specific features.

I chose to use a kernel &lt;-&gt; user comms model based on Alan Cox's psdev
with a char device using read and write and a mmap area for the block
request data (potentially allowing implementation of zero copy in the
future by mapping the bio into the user address space).

It is named 'ub' as it was written way before the USB driver although as
I hadn't published my work no one was aware of this. I should come up
with a new name. Perhaps 'bu'?

This code is from circa ~2002 so don't expect it to compile
