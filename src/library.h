#ifndef LIBRARY_H
#define LIBRARY_H

#define MAX_CORES 8

/* One entry of config.json "launchlist": an alternative launch script. */
typedef struct {
	char name[48];
	char launch[64];
} core_t;

/* One stock emulator: /mnt/SDCARD/Emus/<code>/config.json */
typedef struct {
	char code[32];   /* folder name, e.g. "MD" */
	char name[64];   /* display name, e.g. "Genesis / Mega Drive" */
	char dir[288];   /* emulator folder; launch scripts run from here */
	char launch[64]; /* selected launch script, called with the ROM path */
	char roms[256];  /* resolved ROM folder */
	char ext[160];   /* "zip|bin|md", empty = any non-junk file */
	core_t core[MAX_CORES];
	int n_core;
	int count;       /* entries in the ROM folder, -1 = not scanned */
} emu_t;

typedef void (*scan_fn)(void *ctx, int is_dir, const char *name);

int lib_load_emus(emu_t **out);
int lib_is_rom(const emu_t *e, const char *name);
/* Calls fn (if set) for each subfolder and ROM in dir; returns how many. */
int lib_scan(const emu_t *e, const char *dir, scan_fn fn, void *ctx);
/* Index of the selected launch script in core[], or -1. */
int lib_core_index(const emu_t *e);
/* Select core[i]: rewrites "launch" in config.json, the same field the stock UI uses. */
int lib_set_core(emu_t *e, int i);

#endif
