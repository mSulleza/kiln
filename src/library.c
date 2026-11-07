#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "library.h"
#include "util.h"

#define EMUS_DIR "/mnt/SDCARD/Emus"

static const struct {
	const char *code, *name;
} names[] = {
	{ "ARCADE", "Arcade" },
	{ "ATARI2600", "Atari 2600" },
	{ "ATARI7800", "Atari 7800" },
	{ "CPS1", "CPS-1" },
	{ "CPS2", "CPS-2" },
	{ "CPS3", "CPS-3" },
	{ "DC", "Dreamcast" },
	{ "EASYRPG", "EasyRPG" },
	{ "FBNEO", "FinalBurn Neo" },
	{ "FC", "NES / Famicom" },
	{ "FFMPEG", "Videos" },
	{ "GB", "Game Boy" },
	{ "GBA", "Game Boy Advance" },
	{ "GBC", "Game Boy Color" },
	{ "GG", "Game Gear" },
	{ "GW", "Game & Watch" },
	{ "LYNX", "Atari Lynx" },
	{ "MAME2003PLUS", "MAME 2003-Plus" },
	{ "MAME2010", "MAME 2010" },
	{ "MD", "Genesis / Mega Drive" },
	{ "MS", "Master System" },
	{ "N64", "Nintendo 64" },
	{ "NDS", "Nintendo DS" },
	{ "NEOGEO", "Neo Geo" },
	{ "NGP", "Neo Geo Pocket" },
	{ "OPENBOR", "OpenBOR" },
	{ "PCE", "PC Engine" },
	{ "PICO8", "PICO-8" },
	{ "PPSSPP", "PSP" },
	{ "PS", "PlayStation" },
	{ "SFC", "SNES / Super Famicom" },
	{ "SS", "Saturn" },
	{ "WSC", "WonderSwan" },
};

/* Files that sit next to ROMs but are never games (only checked without an extlist). */
static const char *const junk[] = { "txt", "xml", "db", "torrent", "sqlite", "json", "cfg", "log", "nfo" };

static const char *display_name(const char *code, const char *label)
{
	for (size_t i = 0; i < sizeof names / sizeof *names; i++)
		if (!strcmp(names[i].code, code))
			return names[i].name;
	return label;
}

/* Find the top-level "launch" value, skipping the "launch" keys inside "launchlist". */
static int launch_span(const char *js, const char **start, const char **end)
{
	const char *list = strstr(js, "\"launchlist\"");
	const char *list_end = list ? strchr(list, ']') : NULL;

	for (const char *p = js; (p = strstr(p, "\"launch\"")); p++) {
		if (list && p > list && (!list_end || p < list_end))
			continue;
		const char *v = p + strlen("\"launch\"");
		v += strspn(v, " \t\r\n");
		if (*v++ != ':')
			continue;
		v += strspn(v, " \t\r\n");
		if (*v++ != '"')
			continue;
		const char *q = strchr(v, '"');
		if (!q)
			return -1;
		*start = v;
		*end = q;
		return 0;
	}
	return -1;
}

static void parse_cores(emu_t *e, const char *js)
{
	const char *p = strstr(js, "\"launchlist\"");
	const char *end = p ? strchr(p, ']') : NULL;

	while (end && e->n_core < MAX_CORES && (p = strchr(p, '{')) && p < end) {
		const char *q = strchr(p, '}');
		char obj[256];
		if (!q)
			break;
		snprintf(obj, sizeof obj, "%.*s", (int)(q - p + 1), p);
		core_t *c = &e->core[e->n_core];
		if (!json_str(obj, "name", c->name, sizeof c->name) &&
		    !json_str(obj, "launch", c->launch, sizeof c->launch))
			e->n_core++;
		p = q + 1;
	}
}

int lib_load_emus(emu_t **out)
{
	DIR *d = opendir(EMUS_DIR);
	struct dirent *de;
	emu_t *v = NULL;
	int n = 0, cap = 0;

	while (d && (de = readdir(d))) {
		char path[PATH_MAX], real[PATH_MAX], js[8192], label[64], rel[256];
		const char *ls, *le;
		emu_t e = { .count = -1 };

		if (de->d_name[0] == '.' || de->d_name[0] == '_')
			continue;
		snprintf(e.dir, sizeof e.dir, EMUS_DIR "/%s", de->d_name);
		snprintf(path, sizeof path, "%s/config.json", e.dir);
		if (read_file(path, js, sizeof js) < 0 || launch_span(js, &ls, &le) ||
		    json_str(js, "rompath", rel, sizeof rel))
			continue;
		snprintf(e.launch, sizeof e.launch, "%.*s", (int)(le - ls), ls);
		parse_cores(&e, js);
		if (rel[0] == '/')
			snprintf(path, sizeof path, "%s", rel);
		else
			snprintf(path, sizeof path, "%s/%s", e.dir, rel);
		if (!realpath(path, real))
			continue;
		snprintf(e.roms, sizeof e.roms, "%s", real);
		json_str(js, "extlist", e.ext, sizeof e.ext);
		if (json_str(js, "label", label, sizeof label))
			snprintf(label, sizeof label, "%s", de->d_name);
		snprintf(e.code, sizeof e.code, "%s", de->d_name);
		snprintf(e.name, sizeof e.name, "%s", display_name(e.code, label));

		if (n == cap) {
			cap = cap ? cap * 2 : 32;
			emu_t *nv = realloc(v, cap * sizeof *v);
			if (!nv)
				break;
			v = nv;
		}
		v[n++] = e;
	}
	if (d)
		closedir(d);
	*out = v;
	return n;
}

int lib_is_rom(const emu_t *e, const char *name)
{
	const char *dot = strrchr(name, '.');

	if (!e->ext[0]) {
		for (size_t i = 0; dot && i < sizeof junk / sizeof *junk; i++)
			if (!strcasecmp(dot + 1, junk[i]))
				return 0;
		return 1;
	}
	if (!dot)
		return 0;
	size_t len = strlen(dot + 1);
	for (const char *p = e->ext; *p;) {
		const char *bar = strchr(p, '|');
		size_t n = bar ? (size_t)(bar - p) : strlen(p);
		if (n == len && !strncasecmp(p, dot + 1, n))
			return 1;
		if (!bar)
			break;
		p = bar + 1;
	}
	return 0;
}

int lib_core_index(const emu_t *e)
{
	for (int i = 0; i < e->n_core; i++)
		if (!strcmp(e->core[i].launch, e->launch))
			return i;
	return -1;
}

int lib_set_core(emu_t *e, int i)
{
	char path[PATH_MAX], tmp[PATH_MAX + 4], js[8192];
	const char *a, *b;
	int n, ok;

	if (i < 0 || i >= e->n_core)
		return -1;
	snprintf(path, sizeof path, "%s/config.json", e->dir);
	n = read_file(path, js, sizeof js);
	/* A full buffer may be a truncated file; never write that back. */
	if (n < 0 || n >= (int)sizeof js - 1 || launch_span(js, &a, &b))
		return -1;

	/* Write a temp file and rename it over, so a power cut never leaves half a config. */
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "we");
	if (!f)
		return -1;
	ok = fprintf(f, "%.*s%s%s\n", (int)(a - js), js, e->core[i].launch, b) > 0;
	ok = fflush(f) == 0 && ok;
	ok = fsync(fileno(f)) == 0 && ok;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp, path)) {
		unlink(tmp);
		return -1;
	}
	snprintf(e->launch, sizeof e->launch, "%s", e->core[i].launch);
	return 0;
}

int lib_scan(const emu_t *e, const char *dir, scan_fn fn, void *ctx)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	int n = 0;

	if (!d)
		return 0;
	while ((de = readdir(d))) {
		int is_dir = de->d_type == DT_DIR;
		/* Hidden folders hold per-core saves; Imgs holds box art. */
		if (de->d_name[0] == '.' || !strcmp(de->d_name, "Imgs"))
			continue;
		if (de->d_type == DT_UNKNOWN) {
			char path[PATH_MAX];
			struct stat st;
			snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
			is_dir = !stat(path, &st) && S_ISDIR(st.st_mode);
		}
		if (!is_dir && !lib_is_rom(e, de->d_name))
			continue;
		if (fn)
			fn(ctx, is_dir, de->d_name);
		n++;
	}
	closedir(d);
	return n;
}
