.PHONY: all clean install

CFLAGS=-Wall -Wextra -Wshadow -O2 -ggdb -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200112L -D_XOPEN_SOURCE=600 -D_ISOC99_SOURCE -DVERBOSE #-DFORK
LDFLAGS=-Wl,--as-needed
MQ_LIBS=-lrt
SD_LIBS=-lsystemd
INSTALL=install

all: arduinomux
clean:
	rm -f arduinomux

prefix=/usr
exec_prefix=$(prefix)
libdir=$(exec_prefix)/lib

install: all
	$(INSTALL) -m 755 -D arduinomux $(DESTDIR)$(libdir)/arduino-mux/arduinomux

arduinomux: arduinomux.c arduinomux.h Makefile
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(MQ_LIBS) $(SD_LIBS)
