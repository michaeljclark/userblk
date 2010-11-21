ifneq ($(KERNELRELEASE),)
obj-m	:= ub.o
else

VERSION = $(shell uname -r)
INSTALL_DIR = /usr/sbin
KERNEL_DIR := /lib/modules/$(shell uname -r)/build
MODULE_DIR := /lib/modules/$(shell uname -r)/
PWD := $(shell pwd)

CFLAGS = -Wall -O2 -D_LARGEFILE_SOURCE -D_GNU_SOURCE

all: ub.ko ubd test_brw

ub.ko: ub.c ub.h
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD)

ubd: ubd.c ub.h
	cc $(CFLAGS) $< -o $@

test_brw: test_brw.c
	cc $(CFLAGS) $< -o $@

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean
	rm -f ubd test_brw TAGS *.symvers *~

install: all
#       XXX This is the new official way, but I haven't tested it.
#	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules_install
	cp ub.ko $(MODULE_DIR)
	cp ubd $(INSTALL_DIR)

TAGS:
	etags *.c *.h

endif
