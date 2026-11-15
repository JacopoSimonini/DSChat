# Native unit tests for mictest's pure helpers (host compiler). Run: make
CC      ?= cc
CFLAGS  := -std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow -I../source \
           -fsanitize=address,undefined -fno-omit-frame-pointer
BIN     := build/run_tests
.PHONY: test clean
test: $(BIN)
	./$(BIN)
$(BIN): test_micutil.c ../source/micutil.c ../source/micutil.h
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ test_micutil.c ../source/micutil.c
clean:
	rm -rf build
