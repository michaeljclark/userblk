#!/bin/sh

if [ -z "$1" ]
then
	echo "Usage: $0 <image> [options]"
	exit 1
fi

IMAGE=$1
shift

egrep '^ub' /proc/modules >/dev/null 2>&1
if expr $? == 0 > /dev/null ; then
	rmmod ub
fi
insmod ./ub.ko || (echo "Failed to load ub.ko"; exit 1)

UBC_DEV=/dev/ubc0
UBB_DEV=/dev/ub0

./ubd $* $UBC_DEV $UBB_DEV $IMAGE
