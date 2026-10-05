# Makefile - builds BOTH the kernel module (mydev.ko) and the user app (app).
# ==========================================================================
#
# Usage:
#   make            -> build mydev.ko and app
#   make module     -> build only the kernel module
#   make app        -> build only the C++ user program
#   make load       -> build, then load the module (asks for sudo)
#   make unload     -> unload the module (asks for sudo)
#   make clean      -> delete all build output
#
# How kernel module builds work (the tricky part):
#   We do NOT compile mydev.c ourselves.  We ask the kernel's own build
#   system ("Kbuild", found in /lib/modules/<version>/build) to do it,
#   because it knows the exact compiler flags the running kernel needs.
#   Kbuild then reads THIS Makefile again, with the variable KERNELRELEASE
#   set.  The `ifneq` below detects which of the two passes we are in.
#
# NOTE: recipe lines (the indented commands) MUST start with a TAB character.

ifneq ($(KERNELRELEASE),)
# ---------------------------------------------------------------------------
# Pass 2: we are being read by Kbuild.  Just tell it what to build:
#   obj-m means "build as a loadable Module"; mydev.o -> mydev.ko
# ---------------------------------------------------------------------------
obj-m := mydev.o

else
# ---------------------------------------------------------------------------
# Pass 1: normal `make` invoked by you.
# ---------------------------------------------------------------------------

# Location of the headers/build files for the CURRENTLY running kernel.
# Installed on Ubuntu by: sudo apt install linux-headers-$(uname -r)
KDIR ?= /lib/modules/$(shell uname -r)/build

# C++ compiler and flags for the user app:
#   -std=c++17   use the C++17 language standard
#   -Wall -Wextra  turn on most useful warnings
#   -O2          optimise
CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2

# Targets that are names of actions, not files.
.PHONY: all module clean load unload

# Default target (first one in the file): build everything.
all: module app

# Build the kernel module by handing over to Kbuild:
#   -C $(KDIR)    change into the kernel build directory
#   M=$(CURDIR)   "our module's source is in this directory"
#   modules       the Kbuild target that builds external modules
module:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

# Build the user program.  It is rebuilt only if app.cpp or the shared
# header changed.  $@ means "the target name" (app).
app: app.cpp mydev_ioctl.h
	$(CXX) $(CXXFLAGS) -o $@ app.cpp

# Convenience wrappers around the load/unload script.
load: all
	sudo ./mydev.sh load

unload:
	sudo ./mydev.sh unload

# Remove everything that was generated.  The leading "-" means "keep going
# even if this command fails" (e.g. kernel headers not installed).
clean:
	-$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f app *.o *.ko *.mod *.mod.c *.mod.o .*.cmd modules.order Module.symvers
	rm -rf .tmp_versions

endif
