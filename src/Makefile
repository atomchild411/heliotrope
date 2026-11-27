# Makefile: build heliotrope.  Plain make(1); IRIX's smake, BSD make and GNU
# make all read it.  The defaults are for IRIX with pkgsrc in /usr/pkg:
#
#   make                    (or: make CC=clang PREFIX=/opt/pkgsrc ...)
#
# WPE WebKit's headers come from PREFIX unless WPE_PREFIX says otherwise (a
# staged build of WebKit).

PREFIX =	/usr/pkg
WPE_PREFIX =	$(PREFIX)
CC =		clang
CFLAGS =	-march=mips4 -O2 -Wall -Wextra -Wno-unused-parameter
CPPFLAGS =	-I$(WPE_PREFIX)/include/wpe-webkit-2.0 \
		-I$(WPE_PREFIX)/include/wpe-webkit-2.0/wpe-platform \
		-I$(PREFIX)/include/glib-2.0 -I$(PREFIX)/lib/glib-2.0/include \
		-I$(PREFIX)/include/libsoup-3.0 -I$(PREFIX)/include
LDFLAGS =	-L$(WPE_PREFIX)/lib -L$(PREFIX)/lib -Wl,-rpath,$(PREFIX)/lib
LIBS =		-lWPEWebKit-2.0 -lgio-2.0 -lgobject-2.0 -lglib-2.0 \
		-licui18n -licuuc -lXext -lX11

OBJS =		main.o window.o draw.o frames.o page.o field.o events.o \
		tabs.o menus.o sites.o clipboard.o downloads.o bytecode.o
HDRS =		heliotrope.h window.h draw.h frames.h page.h field.h events.h \
		tabs.h menus.h sites.h clipboard.h downloads.h bytecode.h

heliotrope: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o heliotrope $(OBJS) $(LIBS)

$(OBJS): $(HDRS)

.SUFFIXES: .c .o
.c.o:
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $<

clean:
	rm -f heliotrope $(OBJS)
