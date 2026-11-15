# Native unit tests for the pure-C modules of the DS app (host compiler,
# no libnds). Run from ds/:  make test
CC      ?= cc
CFLAGS  := -std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow -I. -I../source \
           -fsanitize=address,undefined -fno-omit-frame-pointer
SRCS    := main.c test_textbuf.c test_input.c test_proto.c test_kbd.c test_config.c test_adpcm.c \
           ../source/textbuf.c ../source/input.c ../source/proto.c ../source/kbd.c \
           ../source/config.c ../source/adpcm.c
BIN     := build/run_tests

.PHONY: test clean
test: $(BIN)
	./$(BIN)

$(BIN): $(SRCS) $(wildcard *.h ../source/*.h)
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lm

clean:
	rm -rf build
