#!/bin/sh

check_for_pkg_config() {
    which pkg-config >/dev/null && return

    echo
    echo "Error: could not find pkg-config"
    echo
    echo "Please make sure you have pkg-config installed."
    echo
    exit 1
}

# Find a suitable libtoolize utility
if which libtoolize > /dev/null 2>&1; then
  LIBTOOLIZE=libtoolize
else
  if which glibtoolize > /dev/null 2>&1; then
    LIBTOOLIZE=glibtoolize
  fi
fi

rm -f config.cache
aclocal #-I m4
check_for_pkg_config
${LIBTOOLIZE} --force --copy
autoconf
autoheader
automake -a --add-missing -Wall
( cd src/gtest && autoreconf -fvi; )
( cd src/leveldb && mkdir -p m4 && autoreconf -fvi; )
exit
