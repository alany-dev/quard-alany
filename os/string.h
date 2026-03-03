#ifndef __STRING_H__
#define __STRING_H__

#include "os.h"

//计算字符串的长度
inline size_t strlen(const char *str)
{
    char *ptr = (char *)str;
    while (*ptr != EOS) {
        ptr++;
    }
    return ptr - str;
}

#endif