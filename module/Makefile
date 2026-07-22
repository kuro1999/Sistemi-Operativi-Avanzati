obj-m += syscall_throttle.o

syscall_throttle-objs := main.o device.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

check:
	$(MAKE) -C $(KDIR) M=$(CURDIR) C=1 modules
