/* SPDX-License-Identifier: GPL-3.0-only */
#include <ctype.h>
int isdigit(int v) { return v >= '0' && v <= '9'; }
int islower(int v) { return v >= 'a' && v <= 'z'; }
int isupper(int v) { return v >= 'A' && v <= 'Z'; }
int isalpha(int v) { return islower(v) || isupper(v); }
int isalnum(int v) { return isalpha(v) || isdigit(v); }
int isblank(int v) { return v == ' ' || v == '\t'; }
int iscntrl(int v) { return (v >= 0 && v < 32) || v == 127; }
int isspace(int v) { return v == ' ' || (v >= '\t' && v <= '\r'); }
int isprint(int v) { return v >= 32 && v <= 126; }
int isgraph(int v) { return v >= 33 && v <= 126; }
int ispunct(int v) { return isgraph(v) && !isalnum(v); }
int isxdigit(int v) { return isdigit(v) || (v >= 'a' && v <= 'f') || (v >= 'A' && v <= 'F'); }
int tolower(int v) { return isupper(v) ? v - 'A' + 'a' : v; }
int toupper(int v) { return islower(v) ? v - 'a' + 'A' : v; }
