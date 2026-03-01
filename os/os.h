#ifndef __OS_H__
#define __OS_H__

#include <stddef.h>
#include <stdarg.h>

extern int printf(const char* s, ...);
extern void panic(char *s);
extern void sbi_console_putchar(int ch);


#endif