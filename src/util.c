#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util.h"

int read_file(const char *path, char *buf, int sz)
{
	FILE *f = fopen(path, "re");

	if (!f)
		return -1;
	int n = fread(buf, 1, sz - 1, f);
	fclose(f);
	buf[n] = 0;
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
		buf[--n] = 0;
	return n;
}

long read_int(const char *path, long def)
{
	char b[32];

	return read_file(path, b, sizeof b) > 0 ? atol(b) : def;
}

/* Pull a string value out of a flat JSON object; good enough for stock config.json. */
int json_str(const char *js, const char *key, char *out, int outsz)
{
	char pat[64];
	int n = 0;

	snprintf(pat, sizeof pat, "\"%s\"", key);
	const char *p = strstr(js, pat);
	if (!p)
		return -1;
	p += strlen(pat);
	p += strspn(p, " \t\r\n");
	if (*p++ != ':')
		return -1;
	p += strspn(p, " \t\r\n");
	if (*p++ != '"')
		return -1;
	while (*p && *p != '"' && n < outsz - 1) {
		if (*p == '\\' && p[1])
			p++;
		out[n++] = *p++;
	}
	out[n] = 0;
	return n ? 0 : -1;
}

uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
