CFLAGS += -g -W -Wall -Wstrict-prototypes -Wno-unused-parameter -Wwrite-strings -DHAVE_CONFIG_H
# Comment if you don't need statically linked.
STATIC = -static
PREFIX = /usr/local
DATADIR = $(PREFIX)/share/kcgi
MANDIR = $(PREFIX)/man/man3
LIBDIR = $(PREFIX)/lib
INCLUDEDIR = $(PREFIX)/include
VERSION = 0.3.3
LIBOBJS = kcgi.o \
	  compat-memmem.o \
	  compat-reallocarray.o \
	  compat-strlcat.o \
	  compat-strlcpy.o \
	  compat-strtonum.o \
	  input.o \
	  sandbox.o \
	  sandbox-darwin.o \
	  sandbox-systrace.o \
	  wrappers.o
TESTS = test-memmem.c \
	test-reallocarray.c \
	test-sandbox_init.c \
	test-strlcat.c \
	test-strlcpy.c \
	test-strtonum.c \
	test-systrace.c \
	test-zlib.c
SRCS = compat-memmem.c \
       compat-reallocarray.c \
       compat-strlcat.c \
       compat-strlcpy.c \
       compat-strtonum.c \
       extern.h \
       input.c \
       kcgi.c \
       kcgi.h \
       sample.c \
       sandbox.c \
       sandbox-darwin.c \
       sandbox-systrace.c \
       wrappers.c \
       $(TESTS)
WWWDIR = /usr/vhosts/kristaps.bsd.lv/www/htdocs/kcgi

all: libkcgi.a

mime2c: mime2c.o
	$(CC) -o $@ mime2c.o -lutil

libkcgi.a: $(LIBOBJS)
	$(AR) rs $@ $(LIBOBJS)

$(LIBOBJS) sample.o: kcgi.h

$(LIBOBJS): config.h extern.h

config.h: config.h.pre config.h.post configure $(TESTS)
	rm -f config.log
	CC="$(CC)" CFLAGS="$(CFLAGS)" ./configure

installcgi: sample 
	install -m 0755 sample $(PREFIX)/sample.cgi
	install -m 0755 sample $(PREFIX)
	install -m 0444 template.xml $(PREFIX)

install: libkcgi.a
	sed -e "s!@VERSION@!$(VERSION)!g" -e "s!@DATADIR@!$(DATADIR)!g" kcgi.h >kcgi.h~
	sed -e "s!@VERSION@!$(VERSION)!g" -e "s!@DATADIR@!$(DATADIR)!g" kcgi.3 >kcgi.3~
	mkdir -p $(LIBDIR)
	mkdir -p $(INCLUDEDIR)
	mkdir -p $(DATADIR)
	mkdir -p $(MANDIR)
	install -m 0444 libkcgi.a $(LIBDIR)
	install -m 0444 kcgi.h~ $(INCLUDEDIR)/kcgi.h
	install -m 0444 kcgi.3~ $(MANDIR)/kcgi.3
	install -m 0444 template.xml sample.c $(DATADIR)
	rm -f kcgi.h~ kcgi.3~

sample: sample.o libkcgi.a
	$(CC) -o $@ $(STATIC) sample.o -L. -lkcgi -lz

www: index.html kcgi-$(VERSION).tgz kcgi.3.html

installwww: www
	mkdir -p $(WWWDIR)/snapshots
	install -m 0444 index.html kcgi.3.html $(WWWDIR)
	install -m 0444 sample.c $(WWWDIR)/sample.c.txt
	install -m 0444 kcgi-$(VERSION).tgz $(WWWDIR)/snapshots/
	install -m 0444 kcgi-$(VERSION).tgz $(WWWDIR)/snapshots/kcgi.tgz

index.html: index.xml
	sed "s!@VERSION@!$(VERSION)!g" index.xml >$@

kcgi.3.html: kcgi.3
	sed -e "s!@VERSION@!$(VERSION)!g" -e "s!@DATADIR@!$(DATADIR)!g" kcgi.3 | mandoc -Thtml >$@

kcgi-$(VERSION).tgz:
	mkdir -p .dist/kcgi-$(VERSION)
	cp $(SRCS) .dist/kcgi-$(VERSION)
	cp Makefile kcgi.3 template.xml .dist/kcgi-$(VERSION)
	cp configure config.h.pre config.h.post .dist/kcgi-$(VERSION)
	(cd .dist && tar zcf ../$@ kcgi-$(VERSION))
	rm -rf .dist

clean:
	rm -f kcgi-$(VERSION).tgz index.html kcgi.3.html sample
	rm -f libkcgi.a $(LIBOBJS)
	rm -f config.log config.h
	rm -f test-memmem test-memmem.o 
	rm -f test-reallocarray test-reallocarray.o 
	rm -f test-sandbox_init test-sandbox_init.o
	rm -f test-strlcat test-strlcat.o
	rm -f test-strlcpy test-strlcpy.o
	rm -f test-strtonum test-strtonum.o
	rm -f test-systrace test-systrace.o
	rm -f test-zlib test-zlib.o 
	rm -rf *.dSYM
