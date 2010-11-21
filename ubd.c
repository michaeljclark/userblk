#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/signal.h>
#include <sys/user.h>
#include <sys/wait.h>

#include "ub.h"


static volatile int running;

static void sig_handler(int sig)
{
  switch(sig) {
  case SIGTERM:
  case SIGINT:
    running = 0;
    fprintf(stderr,"caught signal, exiting after current request\n");
    break;
  }
}


/* command line options */
char *opt_ubc_device;
char *opt_ubb_device;
char *opt_image;
static int opt_nice = 1;
static int opt_size = 0;
static int opt_debug = 0;


static struct option long_options[] = {
  {"size", 1, 0, 's'},
  {"nice", 1, 0, 'n'},
  {"debug", 0, 0, 'd'},
  {"help", 0, 0, 'h'},
  {0, 0, 0, 0}
};


static void print_usage(char *prog)
{
  fprintf(stderr, "Usage: %s [options] <ubc_dev> <ubb_dev> <image>\n\n"
	  "-h|--help          show usage info\n"
	  "-d|--debug         switch on debug output\n"
	  "-n|--nice <n>      nice level\n"
	  "-s|--size <n>      size in kilobytes of the image\n", prog);
}


static void parse_cmdline(int argc, char **argv)
{
  int c, option_index = 0;

  while ((c = getopt_long(argc, argv, "hdn:s:",
			 long_options, &option_index)) != -1) switch(c) {

    case 's':
      if (sscanf(optarg, "%d", &opt_size) != 1 || opt_size <= 0) {
	fprintf(stderr, "size must be a positive integer\n");
	exit(1);
      }
      break;
    case 'n':
      if (sscanf(optarg, "%d", &opt_nice) != 1) {
	fprintf(stderr, "nice level must be an integer\n");
	exit(1);
      }
      break;
    case 'd':
      opt_debug++;
      break;
    case 'h':
    case '?':
      print_usage(argv[0]);
      exit(1);
  }

  if (argc - optind != 3) {
    print_usage(argv[0]);
    exit(1);
  }

  opt_ubc_device = argv[optind];
  opt_ubb_device = argv[optind+1];
  opt_image = argv[optind+2];
}


int main(int argc, char **argv)
{
  int ub_fd, bi_fd, ret;
  off_t b;
  char *buf;
  ubc_req req;
  struct pollfd pevent[1];
  int rr_pid;

  parse_cmdline(argc, argv);

  /* only exit while between requests */
  signal(SIGINT, sig_handler);
  signal(SIGTERM, sig_handler);

  /* open the ub char device */
  if ((ub_fd = open(opt_ubc_device, O_RDWR)) < 0) {
    perror("ubd: opening ubc device");
    exit(1);
  }

  /* open the disk image */
  if ((bi_fd = open64(opt_image, O_RDWR | O_CREAT | O_LARGEFILE | O_DIRECT,
                     0644)) < 0) {
    perror("ubd: opening image");
    exit(1);
  }

  /* find it's size if not specified */
  if (!opt_size) {
    if (ioctl(bi_fd, BLKGETSIZE, &opt_size) == 0) {
      opt_size /= 2; /* assume 512 byte hardware sectors */
    } else if ((b = lseek(bi_fd, 0, SEEK_END)) > 0) {
      opt_size = b / 1024;
    } else {
      fprintf(stderr, "ubd: can't detect size of block image\n");
      exit(1);
    }
  }

  /* Set the size of the ub device */
  if (ioctl(ub_fd, UBC_IOCSSIZE, &opt_size) < 0) {
    perror("ubb: ioctl(UBC_IOCSSIZE)");
    exit(1);
  }

  /* map the read/write buffer of ub char device */
  if ((buf = mmap(NULL, UB_BUFS * PAGE_SIZE, PROT_READ | PROT_WRITE,
		 MAP_SHARED, ub_fd, 0)) == (void*)-1) {
    perror("ubd: mmap");
    exit(1);
  }

  /* lock me into memory */
  if (mlockall(MCL_CURRENT|MCL_FUTURE)<0) {
    perror("mlockall");
    exit(1);
  }

  /* make sure I run with a high priority */
  if (nice(opt_nice) < 0) {
    perror("ubd: nice");
    exit(1);
  }

  printf("image : %s\n", opt_image);
  printf("size  : %dK\n", opt_size);
  printf("vm    : %p\n", buf);

  /* Get the kernel to reread the partition table */
  if ((rr_pid = fork()) == 0) {
    int bd;
    if ((bd = open(opt_ubb_device, O_RDWR)) < 0) {
      printf("couldn't open block device\n");
      exit(0);
    }
    if (ioctl(bd, BLKRRPART) < 0) {
      printf("error reading partition table\n");
    }
    close(bd);
    exit(0);
  }

  /* okay, start polling the ub char device for requests */
  pevent[0].fd = ub_fd;
  pevent[0].events = POLLIN;
  running = 1;
  while (1) {
    if (!running) {
      exit(0);
    }
    if (rr_pid) {
      /* we only end up reaping after getting an event */
      int status;
     if (waitpid(rr_pid, &status, WNOHANG) == rr_pid)
       rr_pid = 0;
    }

#if 1
    while ((ret = poll(pevent, 1, -1)) < 0 && errno == EINTR) {
      if (!running)
	break;
      if (opt_debug)
	printf("ubd: ubc poll was interuppted, retrying.\n");
    }
    if (!running) break;
    if (ret < 0 && errno != EINTR) {
      perror("ubd: ubc poll");
      continue;
    } else if (!(pevent[0].events & POLLIN)) continue;
#endif

    /* Read the request from ubc */
    while ((ret = read(ub_fd, &req, sizeof(req.in))) < 0 && errno == EINTR) {
      if (opt_debug)
	printf("ubd: ubc read was interuppted, retrying.\n");
    }
    if (ret < 0 && errno != EINTR) {
      perror("ubd: ubc read");
      continue;
    } else if (ret != sizeof(req.in)) {
      fprintf(stderr, "ubd: read request was to short\n");
      continue;
    }

    switch(req.in.cmd) {


    case UB_READ:

      if (opt_debug)
	printf("request[%d]: READ offset=%" PRId64 " size=%zd\n",
	       req.in.seq, req.in.offset, req.in.size);
      b = 0;
      while (b != req.in.size) {
	while ((ret = pread64(bi_fd,
			     buf + req.in.mmap_offset + b,
			     req.in.size - b,
			     req.in.offset + b)) < 0 && errno == EINTR) {
	  if (opt_debug)
	    printf("ubd: img read was interuppted, retrying.\n");
	}
	if (ret < 0 && errno != EINTR) {
	  perror("ubd: image read");
	  break;
	}
	b += ret;
      }
      req.out.result = (b == req.in.size);
      break;


    case UB_WRITE:

      if (opt_debug)
	printf("request[%d]: WRITE offset=%" PRId64 " size=%zd\n",
	       req.in.seq, req.in.offset, req.in.size);
      b = 0;
      while (b != req.in.size) {
	while ((ret = pwrite64(bi_fd,
			      buf + req.in.mmap_offset + b,
			      req.in.size - b,
			      req.in.offset + b)) < 0 && errno == EINTR) {
	  if (opt_debug)
	    printf("ubd: img write was interuppted, retrying.\n");
	}
	if (ret < 0 && errno != EINTR) {
	  perror("ubd: image write");
	  break;
	}
	b += ret;
      }
      req.out.result = (b == req.in.size);
      break;

    default:
      printf("request[%d]: cmd=%d unknown\n",
	     req.in.seq, req.in.cmd);
      break;

    }

    /* Write the response back to ubc */
    while ((ret = write(ub_fd, &req, sizeof(req.out))) < 0 && errno == EINTR) {
      if (opt_debug)
	printf("ubd: ubc write was interuppted, retrying.\n");
    }
    if (ret < 0 && errno != EINTR) {
      perror("ubd: ubc write");
      continue;
    } else if (ret != sizeof(req.out)) {
      fprintf(stderr, "ubd: write response was to short\n");
      continue;
    }

  }

  return 0;
}
