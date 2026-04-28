#
# Copyright (c) 2024, Advanced Micro Devices, Inc. All rights reserved.
# See file LICENSE for terms.
#

if HAVE_IONIC_RDMA

module_LTLIBRARIES        = libuct_ib_ionic.la
libuct_ib_ionic_la_CPPFLAGS = $(BASE_CPPFLAGS) $(IBVERBS_CPPFLAGS) \
                              $(IONIC_RDMA_CPPFLAGS)
libuct_ib_ionic_la_CFLAGS   = $(BASE_CFLAGS) $(LT_CFLAGS)
libuct_ib_ionic_la_LIBADD   = $(top_builddir)/src/ucs/libucs.la \
                              $(top_builddir)/src/uct/libuct.la \
                              $(top_builddir)/src/uct/ib/libuct_ib.la
libuct_ib_ionic_la_LDFLAGS  = $(IONIC_RDMA_LIB) $(IONIC_RDMA_LDFLAGS) \
                              $(IBVERBS_LDFLAGS) \
                              -version-info $(SOVERSION)

libuct_ib_ionic_la_SOURCES  = ionic_rc_md.c \
                              ionic_rc_iface.c \
                              ionic_rc_ep.c

noinst_HEADERS              = ionic_rc.h \
                              ionic_rc_impl.h

include $(top_srcdir)/config/module.am

endif # HAVE_IONIC_RDMA
