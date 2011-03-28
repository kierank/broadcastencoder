# Makefile

include config.mak

all: default

SRCS = obe.c \
       input/lavf/lavf.c \
       encoders/video/avc/x264.c \
       mux/ts/ts.c \
       output/network.c output/rtp/rtp.c output/udp/udp.c

SRCCPP =

SRCCLI = obecli.c

SRCSO =

CONFIG := $(shell cat config.h)

# GPL-only files
ifneq ($(findstring HAVE_GPL 1, $(CONFIG)),)
SRCCLI +=
endif

OBJS = $(SRCS:%.c=%.o)
OBJCLI = $(SRCCLI:%.c=%.o)
OBJSO = $(SRCSO:%.c=%.o)
DEP  = depend

.PHONY: all default fprofiled clean distclean install uninstall dox test testclean

default: $(DEP) obecli$(EXE)

libobe.a: .depend $(OBJS) $(OBJASM)
	$(AR) rc libobe.a $(OBJS) $(OBJASM)
	$(RANLIB) libobe.a

$(SONAME): .depend $(OBJS) $(OBJASM) $(OBJSO)
	$(CC) -shared -o $@ $(OBJS) $(OBJASM) $(OBJSO) $(SOFLAGS) $(LDFLAGS)

obecli$(EXE): $(OBJCLI) libobe.a
	$(CC) -o $@ $+ $(LDFLAGSCLI) $(LDFLAGS)

.depend: config.mak
	@rm -f .depend
	@$(foreach SRC, $(SRCS) $(SRCCLI) $(SRCSO), $(CC) $(CFLAGS) $(SRC) -MT $(SRC:%.c=%.o) -MM -g0 1>> .depend;)

config.mak:
	./configure

depend: .depend
ifneq ($(wildcard .depend),)
include .depend
endif

SRC2 = $(SRCS) $(SRCCLI)

clean:
	rm -f $(OBJS) $(OBJASM) $(OBJCLI) $(OBJSO) $(SONAME) *.a obe obe.exe .depend TAGS
	rm -f checkasm checkasm.exe tools/checkasm.o tools/checkasm-a.o
	rm -f $(SRC2:%.c=%.gcda) $(SRC2:%.c=%.gcno)
	- sed -e 's/ *-fprofile-\(generate\|use\)//g' config.mak > config.mak2 && mv config.mak2 config.mak

distclean: clean
	rm -f config.mak config.h config.log obe.pc
	rm -rf test/

install: x264$(EXE) $(SONAME)
	install -d $(DESTDIR)$(bindir)
	install -d $(DESTDIR)$(includedir)
	install -d $(DESTDIR)$(libdir)
	install -d $(DESTDIR)$(libdir)/pkgconfig
	install -m 644 libobe.a $(DESTDIR)$(libdir)
	install -m 644 obe.pc $(DESTDIR)$(libdir)/pkgconfig
	install obecli$(EXE) $(DESTDIR)$(bindir)
	$(RANLIB) $(DESTDIR)$(libdir)/libobe.a
ifeq ($(SYS),MINGW)
	$(if $(SONAME), install -m 755 $(SONAME) $(DESTDIR)$(bindir))
else
	$(if $(SONAME), ln -f -s $(SONAME) $(DESTDIR)$(libdir)/libobe.$(SOSUFFIX))
	$(if $(SONAME), install -m 755 $(SONAME) $(DESTDIR)$(libdir))
endif
	$(if $(IMPLIBNAME), install -m 644 $(IMPLIBNAME) $(DESTDIR)$(libdir))

uninstall:
	rm -f $(DESTDIR)$(includedir)/obe.h $(DESTDIR)$(libdir)/libobe.a
	rm -f $(DESTDIR)$(bindir)/obe$(EXE) $(DESTDIR)$(libdir)/pkgconfig/obe.pc
	$(if $(SONAME), rm -f $(DESTDIR)$(libdir)/$(SONAME) $(DESTDIR)$(libdir)/libobe.$(SOSUFFIX))

etags: TAGS

TAGS:
	etags $(SRCS)
