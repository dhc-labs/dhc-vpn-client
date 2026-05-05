AM_CPPFLAGS = \
	-I$(top_srcdir)/src/libstrongswan \
	-I$(top_srcdir)/src/libcharon \
	-I$(top_srcdir)/src/libipsec

AM_CFLAGS = \
	$(PLUGIN_CFLAGS) \
	-Wno-unknown-pragmas

if MONOLITHIC
noinst_LTLIBRARIES = libstrongswan-kernel-wintun.la
else
plugin_LTLIBRARIES = libstrongswan-kernel-wintun.la
endif

libstrongswan_kernel_wintun_la_SOURCES = \
	kernel_wintun_plugin.h   kernel_wintun_plugin.c  \
	kernel_wintun_ipsec.h    kernel_wintun_ipsec.c   \
	kernel_wintun_router.h   kernel_wintun_router.c  \
	kernel_wintun_capture.h  kernel_wintun_capture.c \
	kernel_wintun_session.h \
	wintun_loader.h          wintun_loader.c \
	wintun.h \
	windivert_loader.h       windivert_loader.c \
	windivert.h

libstrongswan_kernel_wintun_la_LIBADD = \
	$(top_builddir)/src/libipsec/libipsec.la \
	-liphlpapi

libstrongswan_kernel_wintun_la_LDFLAGS = -module -avoid-version
