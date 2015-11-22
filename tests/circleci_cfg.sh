#!/bin/sh

##
## This is a file for matrix build configuration on Circle CI.
## Since circle.yml is hard to write down multi-line case statement,
## we use this external file for this
##

case $CIRCLE_NODE_INDEX in
    0)
	echo "hw"
	echo "export PLATFORM=hw; export MACHINE=x86_64; export TESTS=qemu;\
 export KERNONLY=; export EXTRAFLAGS=; export LINUX=-l" >> $HOME/.bashrc
	;;
    1)
	echo "hw i486"
	echo "export PLATFORM=hw; export MACHINE=i486; export ELF=elf; export TESTS=qemu;\
 export KERNONLY=; export EXTRAFLAGS='-- -F ACLFLAGS=-m32 -F ACLFLAGS=-march=i686'; export LINUX=-l" >> $HOME/.bashrc
	;;
    2)
	echo "hw kernonly"
	echo "export PLATFORM=hw; export MACHINE=x86_64; export TESTS=none;\
export KERNONLY=-k; export EXTRAFLAGS=; export LINUX=-l" >> $HOME/.bashrc
	;;
    3)
	echo "xen kernonly"
	echo "export PLATFORM=xen; export MACHINE=x86_64; export TESTS=none;\
 export KERNONLY=-k; export EXTRAFLAGS=; export LINUX=-l" >> $HOME/.bashrc
	;;
    # The followings are not yet supported for Linux kernel build
    4)
	echo "xen"
	echo "export PLATFORM=xen; export MACHINE=x86_64; export TESTS=none;\
 export KERNONLY=; export EXTRAFLAGS=; export LINUX=-l" >> $HOME/.bashrc
	;;
    5)
	echo "xen i486"
	echo "export PLATFORM=xen; export MACHINE=i486; export ELF=elf; export TESTS=none;\
 export KERNONLY=; export EXTRAFLAGS='-- -F ACLFLAGS=-m32'; export LINUX=-l" >> $HOME/.bashrc
	;;
esac

echo "export CC=gcc-4.8" >> $HOME/.bashrc
echo "export CXX=g++-4.8" >> $HOME/.bashrc

