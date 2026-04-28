# by nakidai (and lomaster)

CFLAGS += -Itelebot-stripped/include
CFLAGS += -ISIA
CFLAGS += -Wall -fPIC -g -O1
CFLAGS.jsonc != pkg-config --cflags json-c
CFLAGS += ${CFLAGS.jsonc}
CFLAGS.sqlite3 != pkg-config --cflags sqlite3
CFLAGS += ${CFLAGS.sqlite3}

LDFLAGS += -Ltelebot-stripped
LDFLAGS += -Wl,-rpath=telebot-stripped
LDFLAGS += -LSIA
LDLIBS += -ltelebot
LDLIBS += -lsia
LDLIBS += -lm
LDLIBS.jsonc != pkg-config --libs json-c
LDLIBS += ${LDLIBS.jsonc}
LDLIBS.sqlite3 != pkg-config --libs sqlite3
LDLIBS += ${LDLIBS.sqlite3}

all: sinaibot

telebot-stripped/libtelebot.so:
	$(MAKE) -C telebot-stripped

SIA/libsia.a:
	$(MAKE) -C SIA

sinaibot.o: sinaibot.c cvector.h

sinaibot: sinaibot.o telebot-stripped/libtelebot.so SIA/libsia.a
	${CC} -o $@ sinaibot.o ${LDFLAGS} ${LDLIBS}

clean:
	rm -f sinaibot.o sinaibot
	$(MAKE) -C telebot-stripped clean
	$(MAKE) -C SIA clean

.PHONY: all clean
