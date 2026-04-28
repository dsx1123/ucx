#
# Copyright (c) 2024, Advanced Micro Devices, Inc. All rights reserved.
#
# See file LICENSE for terms.
#

#
# Ionic RDMA provider support (AMD Pollara pUEC)
#
AC_ARG_WITH([ionic-rdma],
            [AS_HELP_STRING([--with-ionic-rdma(=DIR)],
                [Enable Ionic RDMA device support (default is guess)])],
            [], [with_ionic_rdma=guess])

have_ionic_rdma=no

AS_IF([test "x$with_ib" = "xno"], [with_ionic_rdma=no])

AS_IF([test "x$with_ionic_rdma" != xno],
      [
       save_LDFLAGS="$LDFLAGS"
       save_CFLAGS="$CFLAGS"
       save_CPPFLAGS="$CPPFLAGS"

       AS_IF([test "x$with_ionic_rdma" != "xyes" -a "x$with_ionic_rdma" != "xguess"],
             [IONIC_RDMA_CPPFLAGS="-I$with_ionic_rdma/include"
              IONIC_RDMA_LDFLAGS="-L$with_ionic_rdma/lib -L$with_ionic_rdma/lib64"],
             [IONIC_RDMA_CPPFLAGS=""
              IONIC_RDMA_LDFLAGS=""])

       LDFLAGS="$IBVERBS_LDFLAGS $IONIC_RDMA_LDFLAGS $LDFLAGS"
       CFLAGS="$IBVERBS_CFLAGS $CFLAGS"
       CPPFLAGS="$IBVERBS_CPPFLAGS $IONIC_RDMA_CPPFLAGS $CPPFLAGS"

       have_ionic_rdma=yes
       AC_CHECK_HEADER([infiniband/ionic_dv.h], [], [have_ionic_rdma=no])
       AC_CHECK_LIB([ionic], [ionic_dv_is_ionic_ctx],
                    [AC_SUBST(IONIC_RDMA_LIB, [-lionic])],
                    [have_ionic_rdma=no])

       AS_IF([test "x$have_ionic_rdma" = xyes],
             [
              uct_ib_modules="${uct_ib_modules}:ionic"
              AC_DEFINE([HAVE_IONIC_RDMA], 1, [Ionic RDMA support])
              AC_SUBST(IONIC_RDMA_CPPFLAGS)
              AC_SUBST(IONIC_RDMA_LDFLAGS)
             ],
             [AS_IF([test "x$with_ionic_rdma" != xguess],
                    [AC_MSG_ERROR(Ionic RDMA support requested but libionic or ionic_dv.h not found)])])

       LDFLAGS="$save_LDFLAGS"
       CFLAGS="$save_CFLAGS"
       CPPFLAGS="$save_CPPFLAGS"
      ])


#
# For automake
#
AM_CONDITIONAL([HAVE_IONIC_RDMA], [test "x$have_ionic_rdma" = xyes])

AC_CONFIG_FILES([src/uct/ib/ionic/Makefile])
