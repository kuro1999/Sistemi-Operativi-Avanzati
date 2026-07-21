obj-m += hello_module.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

check:
	$(MAKE) -C $(KDIR) M=$(CURDIR) C=1 modules
