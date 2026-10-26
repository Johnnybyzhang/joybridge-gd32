#include <stddef.h>
size_t strlen(const char *text) {
    const char *end = text;
    while (*end) ++end;
    return (size_t)(end-text);
}
void *memmove(void *dest, const void *source, size_t n) {
    unsigned char *d=dest;
    const unsigned char *s=source;
    if (d<s) { for (size_t i=0;i<n;i++) d[i]=s[i]; }
    else { while (n) { --n; d[n]=s[n]; } }
    return dest;
}
void *memset(void *dest, int c, size_t n) {
    unsigned char *p = dest;
    while (n--)
        *p++ = (unsigned char)c;
    return dest;
}
void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dest;
}
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    while (n--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        ++x;
        ++y;
    }
    return 0;
}
