#!/bin/sh
# Spike: stage the WPE WebKit build and Heliotrope, and wrap them as IRIX
# binary packages (removable with pkg_delete) for atom's local repo.
#
#   ./make-pkg.sh            (from scratch/wpe, after ninja -C wpe-build)
set -e
WS=/Volumes/portable/workspace-claude/IRIX
HERE=$WS/scratch/wpe
S=$WS/scratch/pkgsrc-irix/destdir
HOST=$WS/scratch/pkgsrc-irix/host/sbin
OUT=$WS/scratch/pkgsrc-irix/packages/All
STAGE=$HERE/stage
CC=$WS/scratch/llvm-irix/cross21/bin/clang
STRIP=$WS/scratch/llvm-irix/cross21/bin/llvm-strip

rm -rf "$STAGE"
# cmake --install never builds (ninja install would rebuild at full parallelism).
DESTDIR=$STAGE cmake --install "$HERE/${BUILD:-wpe-build}" >/dev/null
P=$STAGE/opt/pkgsrc
# Strip the libraries and programs (they are huge with symbols).
find "$P/lib" "$P/libexec" "$P/bin" -type f 2>/dev/null | while read f; do
    case "$f" in *.so*|*/libexec/*|*/bin/*) "$STRIP" "$f" 2>/dev/null || true ;; esac
done

# Heliotrope, against the staged WPE and the cross destdir.
I=$S/opt/pkgsrc/include
PKGCONFIG_CFLAGS="-I$P/include/wpe-webkit-2.0 -I$P/include/wpe-webkit-2.0/wpe-platform -I$I/glib-2.0 -I$S/opt/pkgsrc/lib/glib-2.0/include -I$I/libsoup-3.0 -I$I"
mkdir -p "$P/bin"
"$CC" -target mips64-sgi-irix6.5 -march=mips4 -mtune=r10000 --sysroot="$S" -O2 -Wall -Wextra -Wno-unused-parameter $PKGCONFIG_CFLAGS \
    -o "$P/bin/heliotrope" "$WS/code/heliotrope/src/heliotrope.c" \
    -L"$P/lib" -L"$S/opt/pkgsrc/lib" -Wl,-rpath,/opt/pkgsrc/lib \
    -lWPEWebKit-2.0 -lgio-2.0 -lgobject-2.0 -lglib-2.0 -lXext -lX11
"$STRIP" "$P/bin/heliotrope"

cd "$P"
find . \( -type f -o -type l \) | sed 's|^\./||' | sort > "$HERE/PLIST"
cd "$HERE"
echo "WPE WebKit 2.52.6 (Cairo, CLoop) and the Heliotrope browser -- spike" > comment
cat > desc <<EOF
WPE WebKit 2.52.6 built for IRIX 6.5 (n32): Cairo rendering, the CLoop
JavaScript interpreter, WPEPlatform headless on Mesa (softpipe) EGL.
Heliotrope shows it in an X window. Spike build, not from a pkgsrc recipe.
EOF
cat > build-info <<EOF
MACHINE_ARCH=mipseb
OPSYS=IRIX
OS_VERSION=6.5
PKGTOOLS_VERSION=20260227
EOF
sed 's/^/@pkgdep /' <<EOF > deps
glib2>=2.70
libsoup3>=3.0
glib-networking>=2.70
icu>=78.3nb1
harfbuzz-icu>=2.7
cairo>=1.16
fontconfig>=2.13
libepoxy>=1.5
MesaLib>=21
libxkbcommon>=0.4
libgcrypt>=1.7
libtasn1>=4
libwebp>=1
libjpeg-turbo>=2
libxml2>=2.9
sqlite3>=3
png>=1.6
dejavu-ttf>=2
libX11>=1.6
libXext>=1.1
EOF
{ echo "@name wpewebkit-heliotrope-2.52.6"; cat deps; echo "@cwd /opt/pkgsrc"; echo "@owner root"; echo "@group sys"; cat PLIST; } > PLIST.full
rm -f "$OUT/wpewebkit-heliotrope-2.52.6.tgz"
"$HOST/pkg_create" -p "$P" -I /opt/pkgsrc -f PLIST.full -c comment -d desc -B build-info \
    -F gzip "$OUT/wpewebkit-heliotrope-2.52.6.tgz"
ls -la "$OUT/wpewebkit-heliotrope-2.52.6.tgz"
