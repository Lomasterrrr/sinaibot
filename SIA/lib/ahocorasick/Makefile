LIB = libahocorasick.a
CC ?= cc
CFLAGS = -O2 -Wall -std=c99 -Iinclude -pthread
SRCS != ls src/*.c
OBJS = ${SRCS:.c=.o}

all: ${LIB}

${LIB}: ${OBJS}
	${AR} rcs $@ ${OBJS}

clean:
	rm -f src/*.o ${LIB}

.PHONY: all clean
