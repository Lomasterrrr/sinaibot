# by nakidai
CF = clang-format19
CFLAGS += -Itelebot/include
CFLAGS += -Wall -g -O1 -fPIC
CFLAGS.jsonc != pkg-config --cflags json-c
CFLAGS += ${CFLAGS.jsonc}

LDFLAGS += -Ltelebot
LDFLAGS += -Wl,-rpath=telebot
LDLIBS += -l:libtelebot.so
LDLIBS.jsonc != pkg-config --libs json-c
LDLIBS += ${LDLIBS.jsonc}

all: sinaibot

.PHONY: telebot format
telebot/libtelebot.so:
	make -C telebot

format:
	@find . -type f \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' \) -print0 | xargs -0 ${CF} -i || true

sinaibot.c: cvector.h telebot/libtelebot.so

sinaibot: sinaibot.o
	${CC} -o sinaibot ${LDFLAGS} sinaibot.o ${LDLIBS}

clean:
	rm -f sinaibot.o sinaibot
	make -C telebot clean
