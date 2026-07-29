#pragma once

/* panic() prints message and halts (cli; hlt loop). Never returns. */
void panic(const char *fmt, ...) __attribute__((noreturn));

#define assert(cond)                                                          \
    do {                                                                      \
        if (!(cond))                                                          \
            panic("assert failed: %s:%d: %s", __FILE__, __LINE__, #cond);     \
    } while (0)
