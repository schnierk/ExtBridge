# ExtBridge - portable core + command line tool + tests.
# The macOS app/extension is built with macos/build.sh (needs Xcode).

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -Wall -Wextra -Wno-unused-parameter -Wshadow -Icore/include -Icore/src
UNAME := $(shell uname -s)
ifeq ($(shell uname -m),aarch64)
CFLAGS += -march=armv8-a+crc
endif
ifeq ($(UNAME),Darwin)
CFLAGS += -mcpu=apple-m1
endif
LDLIBS += -lpthread

SRC := $(wildcard core/src/*.c)
OBJ := $(patsubst core/src/%.c,build/obj/%.o,$(SRC))

all: build/l2m build/stress build/bigfile build/koio build/libl2m.a

build/obj/%.o: core/src/%.c core/src/*.h core/include/l2m/l2m.h
	@mkdir -p build/obj
	$(CC) $(CFLAGS) -c $< -o $@

build/libl2m.a: $(OBJ)
	ar rcs $@ $^

build/l2m: tools/l2m.c build/libl2m.a
	$(CC) $(CFLAGS) $< build/libl2m.a $(LDLIBS) -o $@

build/stress: tests/stress.c build/libl2m.a
	$(CC) $(CFLAGS) $< build/libl2m.a $(LDLIBS) -o $@

build/bigfile: tests/bigfile.c build/libl2m.a
	$(CC) $(CFLAGS) $< build/libl2m.a $(LDLIBS) -o $@

build/koio: tests/koio.c build/libl2m.a
	$(CC) $(CFLAGS) $< build/libl2m.a $(LDLIBS) -o $@

asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" LDLIBS="-lpthread -fsanitize=address,undefined"

test: all
	tests/run_all.sh

clean:
	rm -rf build

.PHONY: all clean test asan
