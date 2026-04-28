# by nakidai (and lomaster fix)
OBJS += src/telebot.o
OBJS += src/telebot-core.o
OBJS += src/telebot-parser.o
OBJS += src/telebot-forums.o
OBJS += src/telebot-games.o
OBJS += src/telebot-inline.o
OBJS += src/telebot-passport.o
OBJS += src/telebot-payments.o

CFLAGS += -Iinclude -g -fPIC -include libgen.h # -DDEBUG
CFLAGS += -Werror -Wall -Wno-unused-function
CFLAGS.jsonc != pkg-config --cflags json-c
CFLAGS += ${CFLAGS.jsonc}
CFLAGS.libcurl != pkg-config --cflags libcurl
CFLAGS += ${CFLAGS.libcurl}
LDLIBS.jsonc != pkg-config --libs json-c
LDLIBS += ${LDLIBS.jsonc}
LDLIBS.libcurl != pkg-config --libs libcurl
LDLIBS += ${LDLIBS.libcurl}

libtelebot.so: ${OBJS}
	${CC} -o libtelebot.so -shared ${LDFLAGS} ${OBJS} ${LDLIBS}

clean:
	rm -f libtelebot.so ${OBJS}

.PHONY: clean
