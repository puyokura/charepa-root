.PHONY: all tools module clean help

# make predefines CC as "cc" with origin "default", so ?= would never win.
ifeq ($(origin CC),default)
CC := aarch64-linux-gnu-gcc
endif

CFLAGS  ?= -static -O2 -Wall
OUT     ?= build
MODDIR  ?= src/module

TOOL_NAMES := k178 k138 k126 k145 k169 k37 k42 broot3 rw3 hook5 install_hook recov chk lsx gtest modload
PTHREAD_NAMES := k42
TOOLS := $(addprefix $(OUT)/,$(TOOL_NAMES))

all: tools

## tools: build every userspace tool into $(OUT)/
tools: $(TOOLS)

## module: build myroot.ko out-of-tree; KDIR must point at the vendor kernel tree
module:
	@test -n "$(KDIR)" || { echo "KDIR is not set. Example: make module KDIR=\$$HOME/vendor-kernel"; exit 2; }
	$(MAKE) -C $(MODDIR) KDIR=$(KDIR)

$(OUT)/%: src/tools/%.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(if $(filter $*,$(PTHREAD_NAMES)),-pthread) -o $@ $<

clean:
	rm -rf $(OUT)
	-$(MAKE) -C $(MODDIR) clean

help:
	@printf '%s\n' \
	  "make tools                 build all userspace tools into ./build" \
	  "make module KDIR=<tree>    build src/module/myroot.ko against the vendor kernel tree" \
	  "make clean" \
	  "" \
	  "CC defaults to aarch64-linux-gnu-gcc; override with make CC=<compiler>." \
	  "broot3 is the QEMU reference only; on hardware use k178 (or src/tools/hunt2.sh)."
