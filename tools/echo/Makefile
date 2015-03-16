export RTE_SDK=$(PWD)/../../dpdk
export RTE_TARGET=x86_64-native-linuxapp-gcc

ifeq ($(RTE_SDK),)
$(error "Please define RTE_SDK environment variable")
endif

# Default target, can be overriden by command line or environment
RTE_TARGET ?= x86_64-default-linuxapp-gcc

include $(RTE_SDK)/mk/rte.vars.mk

# binary name
APP = echo

# all source are stored in SRCS-y
SRCS-y := echo.c

CFLAGS += -O3 $(USER_FLAGS)
# CFLAGS += -g -Wall $(USER_FLAGS)
#CFLAGS += $(WERROR_FLAGS)

# workaround for a gcc bug with noreturn attribute
# http://gcc.gnu.org/bugzilla/show_bug.cgi?id=12603
ifeq ($(CONFIG_RTE_TOOLCHAIN_GCC),y)
CFLAGS_echo.o += -Wno-return-type
endif

#include $(RTE_SDK)/mk/rte.extlib.mk
include $(RTE_SDK)/mk/rte.extapp.mk
