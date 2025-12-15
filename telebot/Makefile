# by nakidai

OBJS += src/telebot.o
OBJS += src/telebot-core.o
OBJS += src/telebot-parser.o

CFLAGS.jsonc != pkg-config --cflags json-c
CFLAGS += ${CFLAGS.jsonc}
CFLAGS.libcurl != pkg-config --cflags libcurl
CFLAGS += ${CFLAGS.libcurl}
CFLAGS += -Iinclude -fPIC -DDEBUG
CFLAGS += -Werror -Wall -Wno-unused-function

LDLIBS.jsonc != pkg-config --libs json-c
LDLIBS += ${LDLIBS.jsonc}
LDLIBS.libcurl != pkg-config --libs libcurl
LDLIBS += ${LDLIBS.libcurl}

libtelebot.so: ${OBJS}
	cc -o libtelebot.so -shared ${LDFLAGS} ${OBJS} ${LDLIBS}

clean:
	rm -f libtelebot.so ${OBJS}
