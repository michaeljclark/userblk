#ifndef __UB_H__
#define __UB_H__

#define UB_DEVS 4          /* ubc0 through ubc3, ub0 through ub3 */
#define UB_HARD 1          /* 1 = block requests if userspace dies
			      0 = instead of error out requests */
#define UB_BUFS 32         /* mmap buffer pages */
#define UBC_MAJOR 0        /* dynamic major by default */
#define UBB_MAJOR 0        /* dynamic major by default */
#define UBB_MINORS 16
#define UBB_MINOR_SHIFT 4
#define UBB_BLKSIZE 512    /* 512 byte block size */
#define UBB_HARDSECT 512   /* 512 byte hard sectors */
#define UBB_MAXSECTS 64    /* max sectors per request */

#ifdef __KERNEL__


#ifdef UB_DEBUG
#define PDEBUG(fmt, args...) printk( KERN_WARNING "ub: " fmt, ## args)
#else
#define PDEBUG(fmt, args...) /* not debugging: nothing */
#endif


typedef struct ub_dev {
	/* chardev stuff */
	int                   cdev_inuse;
	struct semaphore      sem;
	void                  **bufs;
	int                   num_bufs;
	struct vm_area_struct *vma;
	int                   vmas;

	/* bdev --> userspace request queue */
	u_long		      seq;
	wait_queue_head_t     waitreq;
	spinlock_t            qlock;
	struct list_head      pending;
	struct list_head      processing;
	struct list_head      free;

	/* blockdev stuff */
	int                   bdev_inuse;
	int                   size;               /* size is in 1K units */
	int                   blksize;
	int                   maxsect;
	struct request_queue  *queue;
	struct gendisk        *gd;
	struct block_device   *bdev;
	int                   busy;
	make_request_fn	      *real_make_request;
	wait_queue_head_t     waitfree;

	/* statistics*/
	int                   free_reqs;
	int                   pending_reqs;
	int                   processing_reqs;

} ub_dev;

#endif

/* userspace communications */

#ifdef __KERNEL__
typedef long long off64_t;
#endif

typedef struct ub_in {
	int seq;
	int cmd;
	off64_t offset;
	size_t size;
	size_t mmap_offset; /* unimplemented multithreaded interface */
} ub_in;

typedef struct ub_out {
	int seq;
	int result;
} ub_out;

typedef union {
  ub_in in;
  ub_out out;
} ubc_req;

#ifdef __KERNEL__

typedef struct ub_req {
	struct list_head    chain;
	struct ub_in        in;
	struct ub_out       out;
	struct request*     breq;
} ub_req;

#endif

/* cmds */
#define UB_READ   0
#define UB_WRITE  1


/* ioctls */
#define UBC_IOC_MAGIC  'u'
#define UBC_IOCRESET    _IO(UBC_IOC_MAGIC, 0)
#define UBC_IOCSSIZE    _IOW(UBC_IOC_MAGIC,  1, int)
#define UBC_IOCGSIZE    _IOR(UBC_IOC_MAGIC,  2, int)
#define UBC_IOC_MAXNR 2

#endif
