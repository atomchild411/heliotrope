#!/bin/sh
# build-native.sh: build the tree configure-native.sh made, on IRIX.  Run from
# the same directory; extra arguments go to ninja (e.g. -j6 -k 50).
BUILD=${BUILD:-wpe-build-native}
P=/usr/pkg
PATH=$P/bin:/usr/bin:/bin:/usr/bsd; export PATH
# Code generators (JavaScriptCore's preprocess.pl) run $CC as a preprocessor
# and fall back to /usr/bin/gcc, which IRIX does not have.
CC=$P/bin/clang; CXX=$P/bin/clang++; export CC CXX
exec ninja -C $BUILD "$@"
