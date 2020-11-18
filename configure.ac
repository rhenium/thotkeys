#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([thotkeys], [1.0.0], [k@rhe.jp])
AM_INIT_AUTOMAKE([foreign])
AC_CONFIG_SRCDIR([thotkeys.c])
AC_CONFIG_HEADERS([config.h])

# Checks for programs.
AC_PROG_CC
CFLAGS="$CFLAGS -Wall -Wextra -Wconversion -Wno-parentheses"

# Checks for libraries.
PKG_CHECK_MODULES(X11, [x11])
PKG_CHECK_MODULES(XINPUT, [xi])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
