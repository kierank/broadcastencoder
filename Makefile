# Makefile

include config.mak

all: default

SRCS = obe.c common/lavc.c common/network/udp/udp.c \
       common/linsys/util.c \
       input/sdi/sdi.c input/sdi/ancillary.c input/sdi/vbi.c input/sdi/linsys/linsys.c  \
       filters/video/video.c filters/video/cc.c filters/audio/audio.c \
       encoders/smoothing.c encoders/audio/lavc/lavc.c encoders/video/avc/x264.c \
       mux/smoothing.c mux/ts/ts.c \
       output/ip/ip.c output/asi/linsys/linsys.c

SRCCXX =

SRCCLI = obecli.c

SRCSO =

CONFIG := $(shell cat config.h)

# Optional module sources
ifneq ($(findstring HAVE_LIBTWOLAME 1, $(CONFIG)),)
SRCS += encoders/audio/mp2/twolame.c
endif

ifneq ($(findstring HAVE_DECKLINK 1, $(CONFIG)),)
SRCCXX += input/sdi/decklink/decklink.cpp
endif

# MMX/SSE optims
ifneq ($(AS),)
X86SRC0 = vfilter.asm
X86SRC  = $(X86SRC0:%=filters/video/x86/%)
X86SRC1 = sdi.asm
X86SRC  += $(X86SRC1:%=input/sdi/x86/%)


ifeq ($(ARCH),X86_64)
ARCH_X86 = yes
ASMSRC   = $(X86SRC:-32.asm=-64.asm)
ASFLAGS += -DARCH_X86_64=1 -DHAVE_CPUNOP=1
endif

ifdef ARCH_X86
ASFLAGS += -I$(SRCPATH)/common/x86/
OBJASM  = $(ASMSRC:%.asm=%.o)
$(OBJASM): common/x86/x86inc.asm common/x86/x86util.asm
endif
endif

OBJS = $(SRCS:%.c=%.o)
OBJSCXX = $(SRCCXX:%.cpp=%.o)
OBJCLI = $(SRCCLI:%.c=%.o)
OBJSO = $(SRCSO:%.c=%.o)
DEP  = depend

.PHONY: all default fprofiled clean distclean install uninstall dox test testclean

default: $(DEP) obecli$(EXE)

libobe.a: .depend $(OBJS) $(OBJSCXX) $(OBJASM)
	$(AR) rc libobe.a $(OBJS) $(OBJSCXX) $(OBJASM)
	$(RANLIB) libobe.a

$(SONAME): .depend $(OBJS) $(OBJSCXX) $(OBJASM) $(OBJSO)
	$(CC) -shared -o $@ $(OBJS) $(OBJASM) $(OBJSO) $(SOFLAGS) $(LDFLAGS)

obecli$(EXE): $(OBJCLI) libobe.a
	$(CC) -o $@ $+ $(LDFLAGSCLI) $(LDFLAGS)

%.o: %.asm
	$(AS) $(ASFLAGS) -o $@ $<
	-@ $(if $(STRIP), $(STRIP) -x $@) # delete local/anonymous symbols, so they don't show up in oprofile

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

.depend: config.mak
	@rm -f .depend
	@$(foreach SRC, $(SRCS) $(SRCCLI) $(SRCSO), $(CC) $(CFLAGS) $(SRC) -MT $(SRC:%.c=%.o) -MM -g0 1>> .depend;)
	@$(foreach SRC, $(SRCCXX), $(CXX) $(CXXFLAGS) $(SRC) -MT $(SRCCXX:%.cpp=%.o) -MM -g0 1>> .depend;)

config.mak:
	./configure

depend: .depend
ifneq ($(wildcard .depend),)
include .depend
endif

SRC2 = $(SRCS) $(SRCCLI)

clean:
	rm -f $(OBJS) $(OBJSCXX) $(OBJASM) $(OBJCLI) $(OBJSO) $(SONAME) *.a obecli obecli.exe .depend TAGS
	rm -f $(SRC2:%.c=%.gcda) $(SRC2:%.c=%.gcno)
	- sed -e 's/ *-fprofile-\(generate\|use\)//g' config.mak > config.mak2 && mv config.mak2 config.mak

distclean: clean
	rm -f config.mak config.h config.log
	rm -rf test/

install: obecli$(EXE) $(SONAME)
	install -d $(DESTDIR)$(bindir)
	install -d $(DESTDIR)$(includedir)
	install -d $(DESTDIR)$(libdir)
	install -m 644 libobe.a $(DESTDIR)$(libdir)
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
	rm -f $(DESTDIR)$(bindir)/obecli$(EXE)
	$(if $(SONAME), rm -f $(DESTDIR)$(libdir)/$(SONAME) $(DESTDIR)$(libdir)/libobe.$(SOSUFFIX))

etags: TAGS

TAGS:
	etags $(SRCS)
