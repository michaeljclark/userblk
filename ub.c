/*
 * ub.c - userspace block driver interface
 *
 * Author:      Michael Clark <michael@metaparadigm.com>
 *
 * Based in part on psdev.
 *
 * An implementation of a loadable kernel mode driver providing
 * multiple kernel/user space bidirectional communications links.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * TODO
 *
 * o  verify creds
 * o  mulithreaded requests (mmap buffer management)
 *
 */

/* #define UB_DEBUG 1 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/proc_fs.h>
#include <linux/hdreg.h>
#include <linux/mm.h>
#include <linux/poll.h>
#include <linux/ioctl.h>
#include <linux/blkpg.h>
#include <linux/init.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/page.h>
#include <linux/blkdev.h>

#include "ub.h"

/* module params */
static int ub_devs       = UB_DEVS;
static int ub_hard       = UB_HARD;
static int ub_bufs       = UB_BUFS;
static int ubc_major     = UBC_MAJOR;
static int ubb_major     = UBB_MAJOR;
static int ubb_maxsect   = UBB_MAXSECTS;
static int ub_reqs       = 1024; /* hmmm... */

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("user space block driver interface");
MODULE_AUTHOR("Michael Clark <michael@metaparadigm.com>");

module_param(ub_devs, int, 0);
module_param(ub_hard, int, 0);
module_param(ub_bufs, int, 0);
module_param(ubc_major, int, 0);
module_param(ubb_major, int, 0);
module_param(ubb_maxsect, int, 0);

MODULE_PARM_DESC(ub_devs, "number of devices to initialise (4)");
MODULE_PARM_DESC(ub_hard, "1=block or 0=error when userspace dies (1)");
MODULE_PARM_DESC(ub_bufs, "number of pages for request buffer (32)");
MODULE_PARM_DESC(ubc_major, "char device major (0=dynamic)");
MODULE_PARM_DESC(ubb_major, "block device major (0=dynamic)");
MODULE_PARM_DESC(ubb_maxsect, "maximum number of sectors per request (64)");


static ub_dev *ub_devices = NULL;
static struct class *ubc_class;


/* ubc_free_req
 *
 * Called once userspace has completed a request.
 * Called with qlock held and moves request onto free list.
 */
static void ubc_free_req(ub_dev *dev, ub_req *req)
{
	if (dev->free_reqs >= ub_reqs) {
		kfree(req);
	} else {
		list_add(&req->chain, dev->free.prev);
		dev->free_reqs++;
	}
}


/* ubc_new_req
 *
 * Called from ubb_transfer to get a new ub_req.
 *
 * Called with qlock held and grabs a request from the free list.
 */
static ub_req* ubc_new_req(ub_dev *dev, struct request *breq)
{
	ub_req *req = NULL;
	int cmd = rq_data_dir(breq);

	if (!list_empty(&dev->free)) {
		req = list_entry(dev->free.next, ub_req, chain);
		list_del(&req->chain);
		dev->free_reqs--;
	}

	if (!req) {
		spin_unlock_irq(&dev->qlock);
		req = kmalloc(sizeof(ub_req), GFP_NOIO);
		spin_lock_irq(&dev->qlock);
		if (!req) return NULL;
	}

	req->breq = breq;
	req->in.seq = ++dev->seq;
	req->in.cmd = cmd;
	req->in.offset = (off64_t)blk_rq_pos(breq) << 9;
	req->in.size = blk_rq_sectors(breq) << 9;
	req->in.mmap_offset = 0;

	return req;
}


static void ub_proc_offset(char *buf, char **start, off_t *offset, int *len)
{
	if (*offset == 0) return;
	if (*offset >= *len) {
		*offset -= *len;
		*len = 0;
	} else {
		*start = buf + *offset;
		*offset = 0;
	}
}


static int ub_read_procmem(char *buf, char **start, off_t offset,
			   int count, int *eof, void *data)
{
	int i, len = 0;
	int limit = count - 80; /* Don't print more than this */

	*start = buf;
	len += sprintf(buf+len,
		       "#dev size blks cuse buse pend proc free bufs\n");
	ub_proc_offset (buf, start, &offset, &len);
	for(i=0; i<ub_devs; i++) {
		ub_dev *dev = &ub_devices[i];
		if (down_interruptible (&dev->sem)) return -ERESTARTSYS;

		len += sprintf(buf+len,"ub%i %i %i %i %i %i %i %i %i\n",
			       i, dev->size, dev->blksize,
			       dev->cdev_inuse, dev->bdev_inuse,
			       dev->pending_reqs, dev->processing_reqs,
			       dev->free_reqs, dev->num_bufs);
		ub_proc_offset (buf, start, &offset, &len);
		if (len > limit) goto out;

	out:
		up (&dev->sem);
		if (len > limit)
			break;
	}
	*eof = 1;
	return len;
}


static int ubc_open(struct inode *inode, struct file *file)
{
	int dev_num = iminor(inode);
	ub_dev *dev; /* device information */

	/*  check the device number */
	if (dev_num >= ub_devs) return -ENODEV;
	dev = &ub_devices[dev_num];

	/* make sure only one has me open */
	if (down_interruptible (&dev->sem)) return -ERESTARTSYS;

	if (dev->cdev_inuse) {
		up (&dev->sem);
		return -EBUSY;
	}
	dev->cdev_inuse++;
	up (&dev->sem);
	
	/* and use file->private_data to point to the device data */
	file->private_data = dev;

	return 0;
}


static int ubc_release(struct inode *inode, struct file *file)
{
	ub_dev *dev = file->private_data;
        ub_req *req;
	struct list_head *pos, *q;

	if (down_interruptible (&dev->sem)) return -ERESTARTSYS;

	if (--dev->cdev_inuse) {
		up (&dev->sem);
		return 0;
	}
	up (&dev->sem);

	if (ub_hard) {

		/* move processing requests back onto pending queue */
		PDEBUG("ubc_release: moving processing requests to pending\n");
		spin_lock_irq(&dev->qlock);
		list_for_each_safe(pos, q, &dev->processing) {
			req = list_entry(pos, ub_req, chain);
			list_move(pos, dev->pending.prev);
			dev->processing_reqs--;
			dev->pending_reqs++;
		}
		spin_unlock_irq(&dev->qlock);

	} else {

		/* Error out pending and processing requests */
		PDEBUG("ubc_release: error out pending request\n");
		spin_lock_irq(&dev->qlock);
		list_for_each_safe(pos, q, &dev->processing) {
			req = list_entry(pos, ub_req, chain);
			list_del(pos);
			dev->processing_reqs--;
			__blk_end_request_all(req->breq, -EIO);
			ubc_free_req(dev, req);
		}
		list_for_each_safe(pos, q, &dev->pending) {
			req = list_entry(pos, ub_req, chain);
			list_del(pos);
			dev->pending_reqs--;
			__blk_end_request_all(req->breq, -EIO);
			ubc_free_req(dev, req);
		}
		spin_unlock_irq(&dev->qlock);

	}

	PDEBUG("ubc_release: done.\n");

	return 0;
}


static ssize_t ubc_read(struct file *file, char *buf,
			size_t nbytes, loff_t *off)
{
	DECLARE_WAITQUEUE(wait, current);
	ub_dev *dev = file->private_data;
        ub_req *req;
	ssize_t retval = 0, count = 0;

	if (nbytes == 0) return 0;

	spin_lock_irq(&dev->qlock);
	if (!list_empty(&dev->pending)) goto skip_wait;
	spin_unlock_irq(&dev->qlock);

	if (file->f_flags & O_NONBLOCK) return -EWOULDBLOCK;

	add_wait_queue(&dev->waitreq, &wait);
	set_current_state(TASK_INTERRUPTIBLE);
	while (list_empty(&dev->pending)) {
		if (file->f_flags & O_NONBLOCK) {
			retval = -EAGAIN;
			break;
		}
		if (signal_pending(current)) {
			retval = -ERESTARTSYS;
			break;
		}
		schedule();
	}
	set_current_state(TASK_RUNNING);
	remove_wait_queue(&dev->waitreq, &wait);
	if (retval) goto out;

	spin_lock_irq(&dev->qlock);

 skip_wait:

	req = list_entry(dev->pending.next, ub_req, chain);
	list_move(&req->chain, dev->processing.prev);
	dev->pending_reqs--;
	dev->processing_reqs++;

	spin_unlock_irq(&dev->qlock);

	/* Move the input args into userspace */
	count = sizeof(ub_in);
	if (nbytes < count) {
                printk ("ubc_read: userspace read %ld of %d in message\n",
			(long)nbytes, (int)sizeof(ub_in));
		count = nbytes;
        }

	if (copy_to_user(buf, &req->in, count)) {
		count = 0;
		retval = -EFAULT;
		goto out;
	}

	if (req->in.cmd == UB_WRITE) {
		u_long offset = 0;
		struct bio_vec *bvec;
		struct req_iterator iter;
		rq_for_each_segment(bvec, req->breq, iter) {
			size_t size = bvec->bv_len;
			void *bvec_buf = kmap(bvec->bv_page) + bvec->bv_offset;
			int buf_num = offset >> PAGE_SHIFT;
			if (buf_num >= dev->num_bufs) BUG();
			memcpy(dev->bufs[buf_num] + offset % PAGE_SIZE,
			       bvec_buf, size);
			kunmap(bvec->bv_page);
			offset += size;
		}
	}

 out:
	return (count ? count : retval);

}


static ssize_t ubc_write(struct file *file, const char *buf,
			 size_t nbytes, loff_t *off)
{
	ub_dev *dev = file->private_data;
        ub_req *req = NULL;
        ub_req *tmp;
	ub_out hdr;
	ssize_t retval = 0, count = 0;
	struct list_head *pos, *q;

        /* Peek at the seq */
	if (copy_from_user(&hdr, buf, 2 * sizeof(u_long)))
	        return -EFAULT;

	PDEBUG("ubc_write: pid %d read request id %d result=%d\n", 
	       current->pid, hdr.seq, hdr.result);
        
	/* Look for the message on the processing queue. */
	spin_lock_irq(&dev->qlock);
	list_for_each_safe(pos, q, &dev->processing) {
		tmp = list_entry(pos, ub_req, chain);
		if (tmp->in.seq == hdr.seq) {
			req = tmp;
			list_del(pos);
			dev->processing_reqs--;
			break;
		}
	}
	spin_unlock_irq(&dev->qlock);

	if (!req) {
		printk("ubc_write: request id %d not found\n", hdr.seq);
		return -ESRCH;
	}

	PDEBUG("found request id %d on queue!\n", hdr.seq);

        /* move data into response buffer. */
	if (sizeof(ub_out) < nbytes) {
                printk("ubc_write: too much data: %d:%ld, request id %d\n",
		       (int)sizeof(ub_out), (long)nbytes, hdr.seq);
		nbytes = sizeof(ub_out); /* don't have more space! */
	}
        if (copy_from_user(&req->out, buf, nbytes)) {
		/* error out request */
		blk_end_request_all(req->breq, -EIO);
		retval = -EFAULT;
		goto out;
	}
	count = nbytes;

	if (req->in.cmd == UB_READ) {
		u_long offset = 0;
		struct bio_vec *bvec;
		struct req_iterator iter;
		rq_for_each_segment(bvec, req->breq, iter) {
			size_t size = bvec->bv_len;
			void *bvec_buf = kmap(bvec->bv_page) + bvec->bv_offset;
			int buf_num = offset >> PAGE_SHIFT;
			if (buf_num >= dev->num_bufs) BUG();
			memcpy(bvec_buf,
			       dev->bufs[buf_num] + offset % PAGE_SIZE, size);
			kunmap(bvec->bv_page);
			offset += size;
		}
	}

	if (req->in.cmd == UB_READ || req->in.cmd == UB_WRITE) {
		blk_end_request_all(req->breq, req->out.result ? 0 : -EIO);
	}

out:
	spin_lock_irq(&dev->qlock);
	ubc_free_req(dev, req);
	spin_unlock_irq(&dev->qlock);

        return(count ? count : retval);  
}


static unsigned int ubc_poll(struct file *file, poll_table * wait)
{
	ub_dev *dev = file->private_data;
	unsigned int mask = POLLOUT | POLLWRNORM;

	poll_wait(file, &dev->waitreq, wait);

	if (!list_empty(&dev->pending))
                mask |= POLLIN | POLLRDNORM;

	return mask;
}


static int ubc_ioctl(struct inode *inode, struct file *file,
		     unsigned int cmd, u_long arg)
{
	int err= 0, ret = 0;
	ub_dev *dev = (ub_dev*)file->private_data;

	if (_IOC_TYPE(cmd) != UBC_IOC_MAGIC) return -ENOTTY;
	if (_IOC_NR(cmd) > UBC_IOC_MAXNR) return -ENOTTY;
	if (_IOC_DIR(cmd) & _IOC_READ)
		err = !access_ok(VERIFY_WRITE, (void *)arg, _IOC_SIZE(cmd));
	else if (_IOC_DIR(cmd) & _IOC_WRITE)
		err =  !access_ok(VERIFY_READ, (void *)arg, _IOC_SIZE(cmd));
	if (err) return -EFAULT;

	switch(cmd) {

	case UBC_IOCSSIZE:
		ret = __get_user(dev->size, (int*)arg);
		set_capacity(dev->gd, dev->size << 1);
		break;

	case UBC_IOCGSIZE:
		ret = __put_user(dev->size, (int*)arg);
		break;

	default:
		return -ENOTTY;
	}

	return ret;
}


static void ubc_vma_open(struct vm_area_struct *vma)
{
	ub_dev *dev = (ub_dev *)vma->vm_private_data;
	dev->vmas++;
}


static void ubc_vma_close(struct vm_area_struct *vma)
{
	ub_dev *dev = (ub_dev *)vma->vm_private_data;
	dev->vmas--;
}


static int ubc_vma_fault(struct vm_area_struct *vma,
				  struct vm_fault *vmf)
{
	ub_dev *dev = (ub_dev *)vma->vm_private_data;
	u_long offset = vmf->pgoff;
	struct page *page = NULL;
	void *pageptr = NULL; /* default to "missing" */

	PDEBUG( "ubc_vma_fault: fault @ %08lx [vma %08lx-%08lx] offset=%ld\n",
		(u_long)vmf->virtual_address,
		vma->vm_start, vma->vm_end, offset);

	down(&dev->sem);

	if (offset >= dev->num_bufs) goto err; /* out of range */

	if (dev && dev->bufs && dev->bufs[offset]) pageptr = dev->bufs[offset];

	if (!pageptr) goto err; /* hole or end-of-file */

	/* got it, now increment the count */
	page = virt_to_page(pageptr);
	get_page(page);

	vmf->page = page;

    	up(&dev->sem);
	return 0;

err:
	up(&dev->sem);
	return VM_FAULT_ERROR;
}


static struct vm_operations_struct ubc_vm_ops = {
	.open         = ubc_vma_open,
	.close        = ubc_vma_close,
	.fault        = ubc_vma_fault
};


static int ubc_mmap(struct file *file, struct vm_area_struct *vma)
{
	ub_dev *dev = (ub_dev*)file->private_data;

	if ((vma->vm_pgoff << PAGE_SHIFT) & (PAGE_SIZE-1))                
		return -ENXIO; /* need aligned offsets */	

	/* don't do anything here: "fault" will fill the holes */
	vma->vm_ops = &ubc_vm_ops;
	vma->vm_flags |= VM_RESERVED;
	vma->vm_private_data = dev;
	ubc_vma_open(vma);
	dev->vma = vma;

	return 0;
}


static int ubb_open(struct block_device *bdev, fmode_t mode)
{
	ub_dev *dev = bdev->bd_disk->private_data;

	down(&dev->sem);
	dev->bdev_inuse++;
	if (! dev->bdev) dev->bdev = bdev;
	/* bdget(inode->i_bdev) */
	up(&dev->sem);
	return 0;
}


static int ubb_release(struct gendisk *gendisk, fmode_t mode)
{
	ub_dev *dev = gendisk->private_data;

	down(&dev->sem);
	dev->bdev_inuse--;
	/* bdput(inode->i_bdev) */
	up(&dev->sem);

	return 0;
}


int ubb_check_transfer_size(ub_dev *dev, int sector)
{
	if (sector > (dev->size << 1)) {
		static int count = 0;
		if (count++ < 5)
			printk(KERN_WARNING
			       "ub: request past end of device\n");
		return -EINVAL;
	}
	return 0;
}


/* ubb_transfer
 *
 * Add a request to the userspace queue. Called from ubb_request
 * with qlock held
 */
static int ubb_transfer(ub_dev *dev, struct request *breq)
{
	ub_req *req;

	if (ubb_check_transfer_size(dev, blk_rq_pos(breq) +
				    blk_rq_sectors(breq)))
		return -EINVAL;

	/* Build a request */
	req = ubc_new_req(dev, breq);

	PDEBUG("ubb_transfer: cmd=%d %lld:%d\n",
	       req->in.cmd, req->in.offset, req->in.size);

	if (!req) {
		printk("Failed to allocate ub_req structure\n");
		return -ENOMEM;
	}

	/* Append msg to pending queue */
	list_add(&(req->chain), dev->pending.prev);
	dev->pending_reqs++;

	/* Wake up userspace if it is around */
	spin_unlock_irq(&dev->qlock);
	if (dev->cdev_inuse && waitqueue_active(&dev->waitreq)) {
		wake_up(&dev->waitreq); /* poll race ??? */
	}
	spin_lock_irq(&dev->qlock);

	return 0;
}


/* ubb_request
 *
 * This is the main block layer request function. It shifts requests
 * from the request queue to the userspace queue and then wakes up
 * userspace. If userspace is not around, it will either IO error the
 * requests of queue them to the userspace queue and skip the wakeup.
 *
 * Called from block layer with qlock held.
 */
static void ubb_request(struct request_queue *q)
{
	ub_dev *dev = (ub_dev*)q->queuedata;
	struct request *req;
	int ret;

	if (dev->busy) return;
	dev->busy = 1;

	/* when not hard we do IO errors if userspace is not around */
	if (!dev->cdev_inuse && !ub_hard) {
		/* Error out the requests, userspace is not there */
		while ((req = blk_fetch_request(q)) != NULL)
			__blk_end_request_all(req, -EIO);
	}

	/* Move the requests to the userspace queue */
	while ((req = blk_fetch_request(q)) != NULL) {
		if (! blk_fs_request(req)) {
			__blk_end_request_all(req, -EIO);
			continue;
		}
		/* Transfer one item onto the userspace queue */
		ret = ubb_transfer(dev, req);
		PDEBUG("DIR=%d\n", rq_data_dir(req));
		if (ret < 0)
			__blk_end_request_all(req, -EIO);
	}

	dev->busy = 0;
}


static struct file_operations ubc_fops = {
	.owner		   = THIS_MODULE,
	.read              = ubc_read,
	.write             = ubc_write,
       	.ioctl             = ubc_ioctl,
	.mmap              = ubc_mmap,
	.open              = ubc_open,
	.poll              = ubc_poll,
	.release           = ubc_release,
};


static struct block_device_operations ubb_bdops = {
	.owner             = THIS_MODULE,
	.open              = ubb_open,
	.release           = ubb_release,
};


static int __init ub_init(void)
{
	int i, j;
	int result = -ENOMEM; /* for the possible errors */
	struct list_head *pos, *q;

	if ((result = register_blkdev(ubb_major, "ub")) < 0) {
		goto fail_reg_blk;
	}
	if (ubb_major == 0) ubb_major = result; /* dynamic */

	if ((result = register_chrdev(ubc_major, "ubc", &ubc_fops)) < 0) {
		goto fail_reg_char;
	}
	if (ubc_major == 0) ubc_major = result; /* dynamic */

	ubc_class = class_create(THIS_MODULE, "ubc");
	if (IS_ERR(ubc_class)) {
                goto fail_reg_class;
        }

	ub_devices = kmalloc(ub_devs * sizeof (ub_dev), GFP_KERNEL);
	memset(ub_devices, 0, ub_devs * sizeof (ub_dev));
	if (!ub_devices) {
		goto fail_devices;
	}

	for (i=0; i < ub_devs; i++) {
		ub_dev *dev = &ub_devices[i];

		/* userspace queue initialisation */
		init_waitqueue_head(&dev->waitreq);
		init_waitqueue_head(&dev->waitfree);
		dev->seq = 0;
		INIT_LIST_HEAD(&dev->pending);
		INIT_LIST_HEAD(&dev->processing);
		INIT_LIST_HEAD(&dev->free);
		for(j=0; j< ub_reqs; j++) {
			ub_req *req;
			req = kmalloc(sizeof(char*)*ub_bufs, GFP_KERNEL);
			if (req) {
				dev->free_reqs++;
				list_add(&req->chain, dev->free.prev);
			}
		}
		dev->bufs = kmalloc(sizeof(char*)*ub_bufs, GFP_KERNEL);
		if (!dev->bufs) goto fail_disks;
		memset(dev->bufs, 0, sizeof(char*)*ub_bufs);
		for(j=0; j < ub_bufs; j++) {
			dev->bufs[j] =
				(void *)__get_free_pages(GFP_KERNEL, 0);
			if (!dev->bufs[j]) goto fail_disks;
		}

		/* char dev initialisation */
		sema_init (&dev->sem, 1);
		dev->num_bufs = ub_bufs;
		device_create(ubc_class, NULL,
			      MKDEV(ubc_major, i), NULL, "ubc%d", i);

		/* block dev initialisation */
		PDEBUG("ub_init: initialise queue on ub%d\n", i);
		spin_lock_init(&dev->qlock);
		dev->size = 0;
		dev->blksize = UBB_BLKSIZE;
		dev->maxsect = ubb_maxsect;
		dev->bdev = NULL;
		dev->queue = blk_init_queue(&ubb_request, &dev->qlock);
		if (!dev->queue) goto fail_disks;
		dev->queue->queuedata = dev;
		blk_queue_max_sectors(dev->queue, dev->maxsect);
		blk_queue_logical_block_size(dev->queue, UBB_HARDSECT);
		blk_queue_max_hw_segments(dev->queue, dev->maxsect);
		blk_queue_max_phys_segments(dev->queue, dev->maxsect);

		PDEBUG("ub_init: adding gendisk ub%d\n", i);
		if (!(dev->gd = alloc_disk(UBB_MINORS))) goto fail_disks;
		dev->gd->first_minor = i << UBB_MINOR_SHIFT;
		dev->gd->queue = dev->queue;
		dev->gd->major = ubb_major;
		dev->gd->fops = &ubb_bdops;
		dev->gd->private_data = dev;
		sprintf(dev->gd->disk_name, "ub%d", i);
		set_capacity(dev->gd, 0);

		add_disk(dev->gd);
	}

	create_proc_read_entry("ub", 0, NULL, ub_read_procmem, NULL);

	printk ("<1>ub: initialised %d devs\n", ub_devs);

	return 0; /* succeed */

 fail_disks:
	for (i=0; i < ub_devs; i++) {
		ub_dev *dev = &ub_devices[i];
		ub_req *req;

		list_for_each_safe(pos, q, &dev->free) {
			req = list_entry(pos, ub_req, chain);
			list_del(pos);
			kfree(req);
		}
		if (dev->queue) {
			PDEBUG("ub_init: cleanup queue ub%d\n", i);
			blk_cleanup_queue(dev->queue);
		}
		if (dev->gd) {
			PDEBUG("ub_init: removing gendisk ub%d\n", i);
			del_gendisk(dev->gd);
			put_disk(dev->gd);
		}
		if (dev->bufs) {
			for(j=0; j < dev->num_bufs; j++)
				if (dev->bufs[j])
					free_pages((u_long)dev->bufs[j], 0);
			kfree(dev->bufs);
		}
	}
	kfree(ub_devices);
fail_devices:
	class_destroy(ubc_class);
fail_reg_class:
	unregister_chrdev(ubc_major, "ubc");	
fail_reg_char:
	unregister_blkdev(ubb_major, "ub");
fail_reg_blk:

	return result;
}


static void __exit ub_cleanup(void)
{
	int i;
	struct list_head *pos, *q;

	for (i=0; i < ub_devs; i++) {
		ub_dev *dev = &ub_devices[i];
		ub_req *req;

		if (dev->gd) {
			PDEBUG("ub_init: removing gendisk ub%d\n", i);
			del_gendisk(dev->gd);
			put_disk(dev->gd);
		}
		if (dev->queue) {
			PDEBUG("ub_init: cleanup queue on ub%d\n", i);
			blk_cleanup_queue(dev->queue);
		}

		if (dev->bufs) {
			int j;
			for(j=0; j < dev->num_bufs; j++)
				if (dev->bufs[j])
					free_pages((u_long)dev->bufs[j], 0);
			kfree(dev->bufs);
		}
		/* free all request lists */
		list_for_each_safe(pos, q, &dev->processing) {
			req = list_entry(pos, ub_req, chain);
			list_del(pos);
			printk(KERN_WARNING "ub: freeing request "
			       "in processing list - shouldn't happen");
			kfree(req);
		}
		list_for_each_safe(pos, q, &dev->pending) {
			req = list_entry(pos, ub_req, chain);
			list_del(pos);
			printk(KERN_WARNING "ub: freeing request "
			       "in pending list - shouldn't happen");
			kfree(req);
		}
		list_for_each_safe(pos, q, &dev->free) {
			req = list_entry(pos, ub_req, chain);
			list_del(pos);
			kfree(req);
		}

		device_destroy(ubc_class, MKDEV(ubc_major, i));
	}

	unregister_blkdev(ubb_major, "ub");
	unregister_chrdev(ubc_major, "ubc");

        class_destroy(ubc_class);

	remove_proc_entry("ub", 0);

	kfree(ub_devices);
}


module_init(ub_init);
module_exit(ub_cleanup);
