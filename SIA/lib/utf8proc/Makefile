AR ?= ar
CC ?= cc
CFLAGS = -O2 -fPIC -std=c99
CFLAGS += -Wsign-conversion -Wall -Wextra -Wc++-compat -pedantic
CFLAGS += -DUTF8PROC_EXPORTS

all: libutf8proc.a

utf8proc.o: utf8proc.h utf8proc.c utf8proc_data.c
	${CC} ${CFLAGS} -c -o utf8proc.o utf8proc.c

libutf8proc.a: utf8proc.o
	rm -f $@
	${AR} crs $@ utf8proc.o

clean:
	rm -f utf8proc.o libutf8proc.a

.PHONY: all clean
