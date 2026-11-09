# mdZ80: Z80 emulator.

AUTOMAKE_OPTIONS = foreign subdir-objects

STRIP_FPIC=$(SHELL) '$(top_srcdir)/strip_fPIC.sh'

.asm.lo:
	$(LIBTOOL) $(LIBTOOLFLAGS) --mode=compile --tag=ASM $(STRIP_FPIC) $(NASM) $< -o $@

noinst_LTLIBRARIES = libmdZ80.la

INCLUDES = -I@top_srcdir@/src/
libmdZ80_la_CCASFLAGS	= $(INCLUDES)

libmdZ80_la_CFLAGS	= $(AM_CFLAGS) -static
libmdZ80_la_LDFLAGS	= $(AM_LDFLAGS) -static

libmdZ80_la_SOURCES = \
		mdZ80.c \
		mdZ80_DAA.c \
		mdZ80_INC_DEC.c

if GENS_CORE_C
# Portable C core.
libmdZ80_la_SOURCES += \
		mdZ80_c.c \
		z80core.c
EXTRA_DIST = mdZ80_x86.asm
else
# x86 asm core.
libmdZ80_la_SOURCES += \
		mdZ80_x86.asm
EXTRA_DIST = mdZ80_c.c z80core.c
endif

EXTRA_DIST += \
		z80core_LICENSE.txt \
		z80core_UPSTREAM_COMMIT.txt

noinst_HEADERS = \
		mdZ80.h \
		mdZ80_flags.h \
		mdZ80_opcodes.h \
		mdZ80_DAA.h \
		mdZ80_INC_DEC.h \
		z80core.h
