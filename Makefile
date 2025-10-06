# by nakidai

CFLAGS += -Itelebot/include
CFLAGS += -Wall -g -O1 -fPIC
CFLAGS.jsonc!=pkg-config --cflags json-c
CFLAGS+=${CFLAGS.jsonc}

LDFLAGS += -Ltelebot
LDFLAGS += -Wl,-rpath=telebot
LDLIBS += -l:libtelebot.so
LDLIBS.jsonc!=pkg-config --libs json-c
LDLIBS+=${LDLIBS.jsonc}

all: sinaibot

.PHONY: telebot
telebot/libtelebot.so:
	make -C telebot

sinaibot.c: cvector.h telebot/libtelebot.so

sinaibot: sinaibot.o
	${CC} -o sinaibot ${LDFLAGS} sinaibot.o ${LDLIBS}

clean:
	rm -f sinaibot.o sinaibot
	make -C telebot clean
