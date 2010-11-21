#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <inttypes.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>


char *opt_ubb_device;
char *opt_image;
static int opt_debug = 0;
static int opt_bufsize = 4096;
static int opt_bufalign = 4096;
static int opt_testoffset = 0;
static char opt_testchar = '!';


static struct option long_options[] = {
  {"bufsize", 1, 0, 'b'},
  {"bufalign", 1, 0, 'a'},
  {"testchar", 1, 0, 'c'},
  {"testoffset", 1, 0, 'o'},
  {"debug", 0, 0, 'd'},
  {"help", 0, 0, 'h'},
  {0, 0, 0, 0}
};


static void print_usage(char *prog)
{
  fprintf(stderr, "Usage: %s [options] <ubb_dev> <image>\n\n"
	  "-h|--help              show usage info\n"
	  "-d|--debug             switch on debug output\n"
	  "-b|--bufsize <int>     read/write buffer size\n"
	  "-a|--bufalign <int>    read/write buffer alignment\n"
	  "-o|--testoffset <int>  read/write test offset\n"
	  "-c|--testchar <char>   read/write test character\n", prog);
}


static void parse_cmdline(int argc, char **argv)
{
  int c, option_index = 0;

  while((c = getopt_long(argc, argv, "hdb:a:o:c:",
			 long_options, &option_index)) != -1) switch(c) {

    case 'b':
      if(sscanf(optarg, "%d", &opt_bufsize) != 1 || opt_bufsize <= 0) {
	fprintf(stderr, "bufsize must be a positive integer\n");
	exit(1);
      }
      break;
    case 'a':
      if(sscanf(optarg, "%d", &opt_bufalign) != 1 || opt_bufalign <= 0) {
	fprintf(stderr, "bufalign must be a positive integer\n");
	exit(1);
      }
      break;
    case 'o':
      if(sscanf(optarg, "%d", &opt_testoffset) != 1 || opt_testoffset < 0) {
	fprintf(stderr, "testoffset must be a positive integer\n");
	exit(1);
      }
      break;
    case 'c':
      if(strlen(optarg) != 1) {
	fprintf(stderr, "testchar must be a single character\n");
	exit(1);
      }
      opt_testchar = optarg[0];
      break;
    case 'd':
      opt_debug++;
      break;
    case 'h':
    case '?':
      print_usage(argv[0]);
      exit(1);
  }

  if (argc - optind != 2) {
    print_usage(argv[0]);
    exit(1);
  }

  opt_ubb_device = argv[optind];
  opt_image = argv[optind+1];

}

int main(int argc, char **argv)
{
  int ub_fd, bi_fd, ret;
  char *wrbuf, *rdbuf;

  parse_cmdline(argc, argv);

  /* open the ubb device */
  if((ub_fd = open64(opt_ubb_device, O_RDWR | O_CREAT | O_LARGEFILE | O_DIRECT,
                     0644)) < 0) {
    perror("test_brw: opening ubb");
    exit(1);
  }

  /* open the disk image */
  if((bi_fd = open64(opt_image, O_RDWR | O_CREAT | O_LARGEFILE | O_DIRECT,
                     0644)) < 0) {
    perror("test_brw: opening image");
    exit(1);
  }

  /* allocate buffers */
  wrbuf = memalign(opt_bufalign, opt_bufsize);
  rdbuf = memalign(opt_bufalign, opt_bufsize);
  if(!wrbuf || !rdbuf) {
    perror("test_brw: error allocating buffers");
    exit(1);
  }

  /* fill test buffer */
  memset(wrbuf, opt_testchar, opt_bufsize);

  /* write to block image */
  if((ret = pwrite64(ub_fd, wrbuf, opt_bufsize, opt_testoffset)) < 0) {
    perror("test_brw: error writing to ubb");
    exit(1);
  }
  if(ret != opt_bufsize) {
    fprintf(stderr, "test_brw: short write: ret=%d", ret);
    exit(1);
  }

  /* read back from device */
  if((ret = pread64(bi_fd, rdbuf, opt_bufsize, opt_testoffset)) < 0) {
    perror("test_brw: error reading from image");
    exit(1);
  }
  if(ret != opt_bufsize) {
    fprintf(stderr, "test_brw: short read: ret=%d", ret);
    exit(1);
  }

  /* compare read and write buffers */
  if(memcmp(wrbuf, rdbuf, opt_bufsize) != 0) {
    fprintf(stderr, "test_brw: read and write buffers differ\n");    
  } else {
    printf("SUCCESS\n");
  }

  /* clean up */
  close(bi_fd);
  close(ub_fd);
  free(wrbuf);
  free(rdbuf);

  exit(0);
}
