// Tiny test helpers for the native unit tests (no framework needed).
#pragma once

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

extern int check_failures;
extern int check_count;

#define CHECK(cond) do { \
        check_count++; \
        if (!(cond)) { \
            check_failures++; \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

#define CHECK_STR(a, b) do { \
        check_count++; \
        const char *a_ = (a), *b_ = (b); \
        if (strcmp(a_, b_) != 0) { \
            check_failures++; \
            fprintf(stderr, "%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, a_, b_); \
        } \
    } while (0)

void test_textbuf(void);
void test_input(void);
void test_proto(void);
void test_kbd(void);
void test_config(void);
void test_adpcm(void);
