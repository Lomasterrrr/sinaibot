CFLAGS += -Iinclude -Isrc -Ilib/utf8proc -Ilib/ahocorasick/include
CFLAGS.jsonc != pkg-config --cflags json-c
CFLAGS += ${CFLAGS.jsonc}
CFLAGS.sqlite3 != pkg-config --cflags sqlite3
CFLAGS += ${CFLAGS.sqlite3}
LDLIBS += -lm
LDLIBS.jsonc != pkg-config --libs json-c
LDLIBS += ${LDLIBS.jsonc}
LDLIBS.sqlite3 != pkg-config --libs sqlite3
LDLIBS += ${LDLIBS.sqlite3}
LIBUTF8 = lib/utf8proc/libutf8proc.a
LIBAHO  = lib/ahocorasick/libahocorasick.a
SRCS != ls src/*.c
OBJS = ${SRCS:.c=.o}

all: libsia.a

${LIBUTF8}:
	$(MAKE) -C lib/utf8proc

${LIBAHO}:
	$(MAKE) -C lib/ahocorasick

libsia.a: ${OBJS} ${LIBUTF8} ${LIBAHO}
	@echo "create $@"       > libsia.mri
	@for obj in ${OBJS}; do echo "addmod $$obj" >> libsia.mri; done
	@echo "addlib ${LIBUTF8}" >> libsia.mri
	@echo "addlib ${LIBAHO}"  >> libsia.mri
	@echo "save"             >> libsia.mri
	@echo "end"              >> libsia.mri
	ar -M < libsia.mri
	@rm libsia.mri

clean:
	rm -f src/*.o libsia.a libsia.mri
	$(MAKE) -C lib/utf8proc clean
	$(MAKE) -C lib/ahocorasick clean

.PHONY: all clean
