#!/bin/sh
# Spike: build WPE for IRIX. CPATH adds GIO's Unix headers, which the GLib
# GioUnix component finds but whose include directory never reaches WebKit's
# compile flags (to fix properly in the pkgsrc recipe).
S=/Volumes/portable/workspace-claude/IRIX/scratch/pkgsrc-irix/destdir
export CPATH=$S/opt/pkgsrc/include/gio-unix-2.0
exec ninja -C /Volumes/portable/workspace-claude/IRIX/scratch/wpe/${BUILD:-wpe-build} -k 200 -j5 "$@"
