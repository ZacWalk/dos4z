/* COMSUBS.C — Stub implementations of COMSUBS.LIB functions.
   These are simplified single-byte versions of the DBCS-aware string
   routines from the original COMSUBS library.  Sufficient for building
   the DOS 4.0 commands in an ASCII/SBCS environment. */

#include <ctype.h>
#include <string.h>

int com_toupper(c)
unsigned char c;
{
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 'A';
    return (int)c;
}

char *com_strchr(s, c)
unsigned char *s;
unsigned char c;
{
    while (*s) {
        if (*s == c) return (char *)s;
        s++;
    }
    return (char *)0;
}

unsigned char *com_strrchr(s, c)
unsigned char *s;
unsigned char c;
{
    unsigned char *last = (unsigned char *)0;
    while (*s) {
        if (*s == c) last = s;
        s++;
    }
    return last;
}

unsigned char *com_strupr(s)
unsigned char *s;
{
    unsigned char *p = s;
    while (*p) {
        if (*p >= 'a' && *p <= 'z')
            *p = *p - 'a' + 'A';
        p++;
    }
    return s;
}

unsigned char *com_substr(s, t)
unsigned char *s;
unsigned char *t;
{
    int tlen;
    if (!*t) return s;
    tlen = strlen((char *)t);
    while (*s) {
        if (strncmp((char *)s, (char *)t, tlen) == 0)
            return s;
        s++;
    }
    return (unsigned char *)0;
}

unsigned rctomid(rc)
unsigned rc;
{
    /* Map DOS error codes to message IDs.
       Without the original mapping table, return the code unchanged. */
    return rc;
}
