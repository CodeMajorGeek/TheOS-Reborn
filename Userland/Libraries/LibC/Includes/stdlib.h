#ifndef _STDLIB_H
#define _STDLIB_H

long long strtoll(const char* nptr, char** endptr, int base);
unsigned long long strtoull(const char* nptr, char** endptr, int base);
double strtod(const char* nptr, char** endptr);
float strtof(const char* nptr, char** endptr);

__attribute__((__noreturn__)) void panic(char* s);
__attribute__((__noreturn__)) void abort(void);

#endif
