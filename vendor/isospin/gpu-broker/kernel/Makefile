# SPDX-License-Identifier: MIT
#
# Makefile for nvidia-shim.ko
#
# Usage:
#   make                    - build against running kernel
#   make KDIR=/path/to/src  - build against specific kernel
#   make clean              - clean build artifacts
#   make install            - install to /lib/modules/$(uname -r)/extra/
#

obj-m := nvidia-shim.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# Extra compiler flags
ccflags-y := -Wall -Werror

.PHONY: all clean install

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

# For development: load/unload
.PHONY: load unload reload

load:
	insmod nvidia-shim.ko

unload:
	rmmod nvidia-shim

reload: unload load

# Test with vsock parameters
.PHONY: load-test

load-test:
	@echo "Loading nvidia-shim with vsock connection to host broker..."
	@echo "Parameters: broker_cid (default: 2=host), broker_port (default: 9999)"
	@echo "Example:"
	@echo "  insmod nvidia-shim.ko broker_cid=2 broker_port=9999"

# Quick test - load with defaults (connect to host:9999)
.PHONY: load-default

load-default:
	insmod nvidia-shim.ko broker_cid=2 broker_port=9999
