############################################################################
# SPDX-License-Identifier: Apache-2.0
#
# tiny-edge as a nuttx-apps application: the core, the POSIX port, the
# code ports share (ports/common), what NuttX needs of its own (src/),
# mbedTLS 4, and the sample agent as the program.
#
# nuttx-apps includes this file from apps/external/tiny_edge/Makefile (see
# scripts/build.sh), so objects are built there, not in the checkout.
############################################################################

# This file's own directory, read before any other file is included.
TINY_EDGE_PORT := $(realpath $(dir $(lastword $(MAKEFILE_LIST))))

include $(APPDIR)/Make.defs

empty :=
space := $(empty) $(empty)

TINY_EDGE_ROOT := $(realpath $(TINY_EDGE_PORT)/../..)
GENERATED      := $(CURDIR)/generated

PROGNAME  = $(CONFIG_TINY_EDGE_PROGNAME)
PRIORITY  = $(CONFIG_TINY_EDGE_PRIORITY)
STACKSIZE = $(CONFIG_TINY_EDGE_STACKSIZE)
MODULE    = $(CONFIG_TINY_EDGE)

# --- Generated: tedge_config.h, the CA bundle, and the source list -------

$(shell mkdir -p $(GENERATED) && cmake -DCONFIG=$(TOPDIR)/.config \
	-DOUT=$(GENERATED) -P $(TINY_EDGE_PORT)/tools/generate.cmake >&2)
include $(GENERATED)/sources.mk

# --- mbedTLS 4.1 -----------------------------------------------------------

MBEDTLS_VERSION := 4.1.0
MBEDTLS_SHA256  := 377a09cf8eb81b5fb2707045e5522d5489d3309fed5006c9874e60558fc81d10
MBEDTLS_DIR     := $(patsubst "%",%,$(strip $(CONFIG_TINY_EDGE_MBEDTLS_DIR)))
ifeq ($(MBEDTLS_DIR),)
MBEDTLS_DIR     := $(CURDIR)/mbedtls-$(MBEDTLS_VERSION)
endif
MBEDTLS_TARBALL := mbedtls-$(MBEDTLS_VERSION).tar.bz2

ifeq ($(wildcard $(MBEDTLS_DIR)/library),)
$(shell set -e; cd $(CURDIR); \
	curl -fsSL -o $(MBEDTLS_TARBALL) \
	  https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$(MBEDTLS_VERSION)/$(MBEDTLS_TARBALL); \
	echo "$(MBEDTLS_SHA256)  $(MBEDTLS_TARBALL)" | sha256sum -c - >&2; \
	tar xjf $(MBEDTLS_TARBALL); rm -f $(MBEDTLS_TARBALL))
endif

TF_PSA := $(MBEDTLS_DIR)/tf-psa-crypto
MBEDTLS_SRCDIRS := $(MBEDTLS_DIR)/library $(TF_PSA)/core \
	$(TF_PSA)/drivers/builtin/src $(TF_PSA)/platform \
	$(TF_PSA)/utilities $(TF_PSA)/extras
MBEDTLS_SRCS := $(foreach d,$(MBEDTLS_SRCDIRS),$(wildcard $(d)/*.c))

# --- Sources -------------------------------------------------------------

# The POSIX port as it is: every file, apart from those this port replaces
# (none yet).
POSIX_SRCS := $(wildcard $(TINY_EDGE_ROOT)/ports/posix/src/*.c)
COMMON_SRCS := $(wildcard $(TINY_EDGE_ROOT)/ports/common/src/*.c)
NUTTX_SRCS := $(wildcard $(TINY_EDGE_PORT)/src/*.c)
POSIX_SRCS := $(filter-out $(addprefix $(TINY_EDGE_ROOT)/ports/posix/src/, \
	$(notdir $(NUTTX_SRCS))),$(POSIX_SRCS))

ALL_SRCS := $(TEDGE_CORE_SRCS) $(TEDGE_PROVISIONING_SRCS) $(POSIX_SRCS) \
	$(COMMON_SRCS) $(NUTTX_SRCS) $(MBEDTLS_SRCS)

# Objects are named after the file alone, so no two sources may share a
# name.
DUPLICATES := $(sort $(foreach n,$(notdir $(ALL_SRCS)), \
	$(if $(filter-out 1,$(words $(filter %/$(n),$(ALL_SRCS)))),$(n))))
ifneq ($(DUPLICATES),)
$(error tiny-edge: sources with the same name: $(DUPLICATES))
endif

CSRCS   = $(notdir $(ALL_SRCS))
MAINSRC = agent_main.c
VPATH  += $(subst $(space),:,$(sort $(dir $(ALL_SRCS)) $(TINY_EDGE_PORT)/samples/agent/))

# --- Flags ---------------------------------------------------------------

CFLAGS += ${INCDIR_PREFIX}$(GENERATED)
CFLAGS += ${INCDIR_PREFIX}$(TINY_EDGE_ROOT)/include
CFLAGS += ${INCDIR_PREFIX}$(TINY_EDGE_PORT)/include
CFLAGS += ${INCDIR_PREFIX}$(TINY_EDGE_ROOT)/ports/posix/include
CFLAGS += ${INCDIR_PREFIX}$(TINY_EDGE_ROOT)/ports/posix/src
CFLAGS += ${INCDIR_PREFIX}$(TINY_EDGE_ROOT)/ports/common/include
CFLAGS += ${INCDIR_PREFIX}$(TINY_EDGE_ROOT)/core/src
CFLAGS += ${INCDIR_PREFIX}$(TINY_EDGE_ROOT)/provisioning/src
CFLAGS += ${INCDIR_PREFIX}$(MBEDTLS_DIR)/include
CFLAGS += ${INCDIR_PREFIX}$(TF_PSA)/include
CFLAGS += ${INCDIR_PREFIX}$(TF_PSA)/drivers/builtin/include
CFLAGS += ${INCDIR_PREFIX}$(TF_PSA)/dispatch
CFLAGS += $(foreach d,$(MBEDTLS_SRCDIRS),${INCDIR_PREFIX}$(d))
CFLAGS += ${DEFINE_PREFIX}TEDGE_VERSION_STRING=\"$(TEDGE_VERSION)\"
# The POSIX port's options for a POSIX RTOS: the core's stack sizes, since
# NuttX's default thread stack is a few KB, the MAC address from
# SIOCGIFHWADDR, since NuttX's getifaddrs() has no link-layer entries, and
# no exec().
CFLAGS += ${DEFINE_PREFIX}TEDGE_POSIX_THREAD_STACKS
CFLAGS += ${DEFINE_PREFIX}TEDGE_POSIX_MAC_IOCTL
# A restart resets the board (the sample's restart), and NuttX has exec()
# only with CONFIG_LIBC_EXECFUNCS.
CFLAGS += ${DEFINE_PREFIX}TEDGE_POSIX_NO_EXEC
# mbedTLS's Unix code paths (time, entropy from /dev/urandom, pthreads),
# as nuttx-apps builds mbedTLS 3; the two user configs (include/) add
# threading and the memory settings.
CFLAGS += ${DEFINE_PREFIX}unix
CFLAGS += ${DEFINE_PREFIX}TF_PSA_CRYPTO_USER_CONFIG_FILE=\"tedge_mbedtls_user_config.h\"
CFLAGS += ${DEFINE_PREFIX}MBEDTLS_USER_CONFIG_FILE=\"tedge_mbedtls_tls_config.h\"
CFLAGS += -std=gnu11

include $(APPDIR)/Application.mk
