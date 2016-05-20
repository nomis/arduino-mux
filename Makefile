CFLAGS=-Wall -Wextra -Wshadow -O2 -ggdb -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200112L -D_XOPEN_SOURCE=600 -D_ISOC99_SOURCE -DVERBOSE #-DFORK
LDFLAGS=-Wl,--as-needed
MQ_LIBS=-lrt
.PHONY: all clean
all: arduinomux
clean:
	rm -f arduinomux

arduinomux: arduinomux.c arduinomux.h Makefile
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(MQ_LIBS)
