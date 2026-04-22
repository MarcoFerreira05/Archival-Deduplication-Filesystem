# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
# Standalone Makefile for cachestat BPF tool (x86 only)
# Requires: clang, llvm-strip, bpftool, libbpf-dev (system-installed)

OUTPUT := $(abspath .output)
CLANG ?= clang
LLVM_STRIP ?= llvm-strip
BPFTOOL ?= bpftool
INCLUDES := -I$(OUTPUT)
CFLAGS := -g -O2 -Wall -Wmissing-field-initializers -Werror -Werror=undef
BPFCFLAGS := -g -O2 -Wall -Werror=undef
INSTALL ?= install
prefix ?= /usr/local
bindir := $(prefix)/bin

COMMON_OBJ = \
	$(OUTPUT)/trace_helpers.o \
	$(OUTPUT)/uprobe_helpers.o \
	#

.PHONY: all
all: cachestat

ifeq ($(V),1)
Q =
msg =
else
Q = @
msg = @printf '  %-8s %s%s\n' "$(1)" "$(notdir $(2))" "$(if $(3), $(3))";
MAKEFLAGS += --no-print-directory
endif

ifneq ($(EXTRA_CFLAGS),)
CFLAGS += $(EXTRA_CFLAGS)
endif
ifneq ($(EXTRA_LDFLAGS),)
LDFLAGS += $(EXTRA_LDFLAGS)
endif

.PHONY: clean
clean:
	$(call msg,CLEAN)
	$(Q)rm -rf $(OUTPUT) cachestat cachestat.skel.h

$(OUTPUT):
	$(call msg,MKDIR,$@)
	$(Q)mkdir -p $@

cachestat: $(OUTPUT)/cachestat.o $(COMMON_OBJ) | $(OUTPUT)
	$(call msg,BINARY,$@)
	$(Q)$(CC) $(CFLAGS) $^ $(LDFLAGS) -lbpf -lelf -lz -o $@

vmlinux.h:
	bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h

$(OUTPUT)/cachestat.o: cachestat.skel.h

$(OUTPUT)/%.o: %.c $(wildcard %.h) | $(OUTPUT)
	$(call msg,CC,$@)
	$(Q)$(CC) $(CFLAGS) $(INCLUDES) -c $(filter %.c,$^) -o $@

cachestat.skel.h: $(OUTPUT)/cachestat.bpf.o | $(OUTPUT)
	$(call msg,GEN-SKEL,$@)
	$(Q)$(BPFTOOL) gen skeleton $< > $@

$(OUTPUT)/cachestat.bpf.o: cachestat.bpf.c vmlinux.h | $(OUTPUT)
	$(call msg,BPF,$@)
	$(Q)$(CLANG) $(BPFCFLAGS) -target bpf -D__TARGET_ARCH_x86 \
		     -Ix86/ $(INCLUDES) -c $(filter %.c,$^) -o $@ && \
	$(LLVM_STRIP) -g $@

install: cachestat
	$(call msg,INSTALL,cachestat)
	$(Q)$(INSTALL) -m 0755 -d $(DESTDIR)$(bindir)
	$(Q)$(INSTALL) cachestat $(DESTDIR)$(bindir)/cachestat

# delete failed targets
.DELETE_ON_ERROR:
# keep intermediate (.skel.h, .bpf.o, etc) targets
.SECONDARY:
