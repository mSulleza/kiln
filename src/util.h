#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>

int  read_file(const char *path, char *buf, int sz); /* trims trailing whitespace */
long read_int(const char *path, long def);
int  json_str(const char *js, const char *key, char *out, int outsz);
uint64_t now_ms(void);

#endif
