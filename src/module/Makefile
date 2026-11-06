# out-of-tree build of src/module against a vendor kernel tree
#
# The kernel tree must be configured and prepared for external modules
# (`make ARCH=arm64 olddefconfig && make ARCH=arm64 modules_prepare`),
# and Module.symvers must exist for the symbols below.
#
#   KDIR  path to the kernel source tree (required)
#
# Symbols resolved at load time through kallsyms_lookup_name, so Module.symvers
# only has to contain what the compiler links: kallsyms_lookup_name, printk,
# proc_create_data, remove_proc_entry.

ifneq ($(KDIR),)
obj-m += myroot.o
PWD := $(shell pwd)
all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
else
all clean:
	@echo "KDIR is not set. Example:"
	@echo "  make -C src/module KDIR=\$$HOME/vendor-kernel"
	@exit 2
endif
