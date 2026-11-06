/* Freestanding replacements for the libc string functions we need.
 * (There is no libc in the kernel yet — Phase 5 adds one for userland.) */
#ifndef MIRIX_LIB_STRING_H
#define MIRIX_LIB_STRING_H

#include <stddef.h>

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
size_t strlen(const char *s);
int strncmp(const char *a, const char *b, size_t n);
int memcmp(const void *a, const void *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);

#endif
