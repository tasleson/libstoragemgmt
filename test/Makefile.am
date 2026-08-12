AM_CPPFLAGS = \
	-I$(top_srcdir)/c_binding/include \
	-I@srcdir@/c_binding/include

EXTRA_DIST=cmdtest.py plugin_test.py test_include.sh runtests.sh.in integration.sh

if WITH_TEST
all: tester libsg_test

check_PROGRAMS = tester libsg_test
tester_CFLAGS = $(LIBCHECK_CFLAGS)
tester_LDADD = ../c_binding/libstoragemgmt.la $(LIBCHECK_LIBS)
tester_SOURCES = tester.c

# libsg_test includes c_binding/libsg.c rather than linking the shared
# library: the response parsing it covers is static to that translation unit,
# and the rest is hidden visibility. It needs no device and no daemon.
libsg_test_CFLAGS = $(LIBCHECK_CFLAGS)
libsg_test_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/c_binding
libsg_test_LDADD = $(LIBCHECK_LIBS)
libsg_test_SOURCES = libsg_test.c \
	$(top_srcdir)/c_binding/utils.c \
	$(top_srcdir)/c_binding/libata.c
endif
