#ifneq ($(KERNELRELEASE),)
 xos-objs:=vmx.o xos.o 
#else
 obj-m:=xos.o
KERNELDIR ?=/lib/modules/$(shell uname -r)/build
PWD:=$(shell pwd)
default:
	make -C $(KERNELDIR) M=$(PWD) modules
#endif

.PHONY:clean
clean:
	-rm *.o *.mod.c *.order *.ko *.mod.o *.symvers
