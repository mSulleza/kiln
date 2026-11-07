/* kiln: a minimal launcher for the TrimUI Brick. Draws straight to fb0, reads
 * the "TRIMUI Player1" evdev device, browses ROMs and runs the stock Emus/ and
 * Apps/ launch scripts from the SD card. */
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <ifaddrs.h>
#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/reboot.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "gfx.h"
#include "input.h"
#include "library.h"
#include "util.h"
#include "font.h"

#define APPS_DIR  "/mnt/SDCARD/Apps"
#define PORTS_DIR "/mnt/SDCARD/Ports"
#define KILN_BIN  "/mnt/SDCARD/kiln/bin"
#define KILN_LIB  "/mnt/SDCARD/kiln/lib"
#define STOCK_PATH "/usr/trimui/bin:/usr/sbin:/usr/bin:/sbin:/bin"
#define BAT_DIR   "/sys/class/power_supply/axp2202-battery"
#define MAX_DEPTH 16

#define HEADER_H 76
#define LIST_Y   92
#define FOOTER_Y 700
#define ROW_H    74 /* two-line rows: menus, systems, apps */
#define ROW_H_SM 52 /* one-line rows: ROM lists */
#define KB_Y     404 /* top of the on-screen keyboard */

#define C_BG_TOP 0xFF1C1E23
#define C_BG_BOT 0xFF0C0D10
#define C_ACCENT 0xFFE8743B
#define C_TEXT   0xFFECEAE6
#define C_DIM    0xFF8E8B86
#define C_ROW    0x0FFFFFFF
#define C_LINE   0x1AFFFFFF
#define C_INK    0xFF15161A

/* IT_FOLDER sorts before IT_ROM. */
enum { IT_GAMES, IT_PORTS, IT_APPS, IT_SYSTEM, IT_FOLDER, IT_ROM, IT_APP, IT_INFO, IT_STOCK, IT_REBOOT, IT_POWEROFF };
enum { SCR_LIST, SCR_INFO };

typedef struct {
	int kind, emu;
	char *label, *sub, *path, *cmd;
} item_t;

typedef struct screen {
	char title[64];
	int kind, row_h, n, cap, sel, top;
	int global;               /* Y searches every system, not just this list */
	const struct screen *src; /* set on search results: the list being filtered */
	item_t *it;
} screen_t;

typedef struct {
	int pct, charging;
	char status[24];
} bat_t;

static volatile sig_atomic_t quit; /* 1 = signal, 2 = hand back to the stock UI */
static canvas_t cv;
static screen_t stack[MAX_DEPTH];
static int depth = -1, confirm = -1;
static emu_t *emus;
static int n_emus = -1; /* -1 until the first Games visit */
static screen_t all_games; /* every top-level ROM, built on the first global search */
static int picker = -1, picker_sel; /* emulator picker: emu index while open */
static int kb_open, kb_row = 1, kb_col;
static char query[40];

static const char *const kb_keys[4] = { "1234567890", "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ-'.:" };
static const struct {
	int col, span;
	const char *label;
} kb_wide[4] = { { 0, 4, "Space" }, { 4, 2, "Delete" }, { 6, 2, "Clear" }, { 8, 2, "Done" } };

static void on_signal(int sig)
{
	(void)sig;
	quit = 1;
}

static screen_t *cur(void)
{
	return &stack[depth];
}

static int visible(const screen_t *s)
{
	return ((kb_open ? KB_Y : FOOTER_Y) - LIST_Y) / s->row_h;
}

static screen_t *push(const char *title, int kind, int row_h)
{
	if (depth + 1 == MAX_DEPTH)
		return NULL;
	screen_t *s = &stack[++depth];
	memset(s, 0, sizeof *s);
	snprintf(s->title, sizeof s->title, "%s", title);
	s->kind = kind;
	s->row_h = row_h;
	return s;
}

static void clear_items(screen_t *s)
{
	for (int i = 0; i < s->n; i++) {
		free(s->it[i].label);
		free(s->it[i].sub);
		free(s->it[i].path);
		free(s->it[i].cmd);
	}
	s->n = s->sel = s->top = 0;
}

static void pop(void)
{
	clear_items(cur());
	free(cur()->it);
	depth--;
}

static char *dup_or_null(const char *s)
{
	return s ? strdup(s) : NULL;
}

static void add(screen_t *s, int kind, const char *label, const char *sub, const char *path,
		const char *cmd, int emu)
{
	if (s->n == s->cap) {
		int cap = s->cap ? s->cap * 2 : 16;
		item_t *it = realloc(s->it, cap * sizeof *it);
		if (!it)
			return;
		s->it = it;
		s->cap = cap;
	}
	s->it[s->n++] = (item_t){ kind, emu, dup_or_null(label), dup_or_null(sub), dup_or_null(path),
				  dup_or_null(cmd) };
}

static int cmp_items(const void *a, const void *b)
{
	const item_t *x = a, *y = b;

	if (x->kind != y->kind)
		return x->kind - y->kind;
	return strcasecmp(x->label, y->label);
}

static void sort_items(screen_t *s)
{
	qsort(s->it, s->n, sizeof *s->it, cmp_items);
}

/* MainUI runs "launch" through a shell, so PortMaster writes paths like Cave\ Story.sh. */
static void unescape_shell(char *s)
{
	char *o = s;

	for (; *s; s++)
		if (*s != '\\' || !s[1])
			*o++ = *s;
		else
			*o++ = *++s;
	*o = 0;
}

/* Stock app format, used by both Apps/ and Ports/: <base>/<name>/config.json with "label"
 * and "launch" (relative to that folder, or absolute). */
static void fill_apps(screen_t *s, const char *base, const char *kind)
{
	DIR *d = opendir(base);
	struct dirent *e;

	while (d && (e = readdir(d))) {
		char dir[288], path[320], js[4096], label[64], cmd[256], sub[96];
		if (e->d_name[0] == '.')
			continue;
		snprintf(dir, sizeof dir, "%s/%s", base, e->d_name);
		snprintf(path, sizeof path, "%s/config.json", dir);
		if (read_file(path, js, sizeof js) < 0 || json_str(js, "label", label, sizeof label) ||
		    json_str(js, "launch", cmd, sizeof cmd))
			continue;
		unescape_shell(cmd);
		if (json_str(js, "description", sub, sizeof sub))
			snprintf(sub, sizeof sub, "%s", kind);
		add(s, IT_APP, label, sub, dir, cmd, -1);
	}
	if (d)
		closedir(d);
	sort_items(s);
}

static bat_t battery(void)
{
	bat_t b = { (int)read_int(BAT_DIR "/capacity", -1), 0, "Unknown" };

	read_file(BAT_DIR "/status", b.status, sizeof b.status);
	b.charging = !strcmp(b.status, "Charging");
	return b;
}

static long meminfo(const char *key)
{
	FILE *f = fopen("/proc/meminfo", "re");
	char line[128];
	size_t kl = strlen(key);
	long v = 0;

	while (f && fgets(line, sizeof line, f))
		if (!strncmp(line, key, kl) && line[kl] == ':') {
			v = atol(line + kl + 1);
			break;
		}
	if (f)
		fclose(f);
	return v;
}

static int zone_temp(int i)
{
	char path[64];

	snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", i);
	return (int)(read_int(path, 0) / 1000);
}

static const char *wlan_ip(char *buf, size_t sz)
{
	struct ifaddrs *ifa;

	snprintf(buf, sz, "not connected");
	if (getifaddrs(&ifa))
		return buf;
	for (struct ifaddrs *p = ifa; p; p = p->ifa_next)
		if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET && !strcmp(p->ifa_name, "wlan0"))
			inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_addr)->sin_addr, buf, sz);
	freeifaddrs(ifa);
	return buf;
}

#define ROW(k, ...) (snprintf(r[n][0], 96, "%s", k), snprintf(r[n][1], 96, __VA_ARGS__), n++)

static int info_rows(char (*r)[2][96])
{
	struct utsname u;
	struct statvfs sv;
	char buf[96];
	int n = 0;

	uname(&u);
	ROW("Kernel", "Linux %s %s", u.release, u.machine);
	long up = read_file("/proc/uptime", buf, sizeof buf) > 0 ? atol(buf) : 0;
	ROW("Uptime", "%ldh %02ldm", up / 3600, up / 60 % 60);
	ROW("CPU", "4x Cortex-A53, %ld MHz",
	    read_int("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", 0) / 1000);
	ROW("Temps", "CPU %d C   GPU %d C   DDR %d C", zone_temp(0), zone_temp(1), zone_temp(2));
	ROW("Memory", "%ld MB free of %ld MB", meminfo("MemAvailable") / 1024, meminfo("MemTotal") / 1024);
	bat_t b = battery();
	ROW("Battery", "%d%%, %s, %.2f V", b.pct, b.status, read_int(BAT_DIR "/voltage_now", 0) / 1e6);
	if (!statvfs("/mnt/SDCARD", &sv))
		ROW("SD card", "%.1f GB free of %.1f GB", (double)sv.f_bavail * sv.f_frsize / 1e9,
		    (double)sv.f_blocks * sv.f_frsize / 1e9);
	ROW("Wi-Fi", "%s", wlan_ip(buf, sizeof buf));
	ROW("Display", "%dx%d, fb0", cv.w, cv.h);
	ROW("Last input", "%s", input_last());
	return n;
}

static void draw_battery(int x, int y, const bat_t *b)
{
	int pct = b->pct < 0 ? 0 : b->pct > 100 ? 100 : b->pct;
	uint32_t col = b->charging ? 0xFF5BC27A : pct <= 15 ? 0xFFE5534B : C_TEXT;

	fill_rrect(&cv, x, y, 40, 20, 5, 0x40FFFFFF);
	fill_rrect(&cv, x + 41, y + 6, 3, 8, 1, 0x40FFFFFF);
	if (pct)
		fill_rrect(&cv, x + 3, y + 3, 34 * pct / 100 < 4 ? 4 : 34 * pct / 100, 14, 3, col);
}

static void render_header(const char *crumb)
{
	int cy = HEADER_H / 2, x;
	char s[32], buf[96];

	fill_rrect(&cv, 36, cy - 13, 26, 26, 7, C_ACCENT);
	x = draw_text(&cv, &font_big, 76, text_y(&font_big, cy), "kiln", C_TEXT);
	if (crumb) {
		x = draw_text(&cv, &font_big, x + 14, text_y(&font_big, cy), "/", C_DIM);
		fit_text(&font_big, crumb, cv.w - 280 - (x + 14), buf, sizeof buf); /* stop short of the clock */
		draw_text(&cv, &font_big, x + 14, text_y(&font_big, cy), buf, C_DIM);
	}

	bat_t b = battery();
	x = cv.w - 36 - 44;
	draw_battery(x, cy - 10, &b);
	if (b.pct >= 0)
		snprintf(s, sizeof s, "%d%%", b.pct);
	else
		snprintf(s, sizeof s, "--");
	x -= 12 + text_width(&font_sm, s);
	draw_text(&cv, &font_sm, x, text_y(&font_sm, cy), s, C_TEXT);

	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(s, sizeof s, "%H:%M", &tm);
	x -= 28 + text_width(&font_sm, s);
	draw_text(&cv, &font_sm, x, text_y(&font_sm, cy), s, C_TEXT);

	fill_rect(&cv, 0, HEADER_H, cv.w, 1, C_LINE);
}

/* A button hint: key in a pill, then its label. Returns x for the next chip. */
static int chip(int x, int cy, const char *key, const char *label)
{
	int kw = text_width(&font_sm, key);
	int w = kw + 18 < 30 ? 30 : kw + 18;

	fill_rrect(&cv, x, cy - 15, w, 30, 15, C_TEXT);
	draw_text(&cv, &font_sm, x + (w - kw) / 2, text_y(&font_sm, cy), key, C_INK);
	x = draw_text(&cv, &font_sm, x + w + 10, text_y(&font_sm, cy), label, C_DIM);
	return x + 30;
}

static void render_footer(const screen_t *s)
{
	int cy = (FOOTER_Y + cv.h) / 2, x = 36;
	char right[32];

	fill_rect(&cv, 0, FOOTER_Y, cv.w, 1, C_LINE);
	if (s->kind == SCR_INFO) {
		chip(x, cy, "B", "Back");
		return;
	}
	const item_t *t = s->n ? &s->it[s->sel] : NULL;
	x = chip(x, cy, "A", t && t->kind == IT_ROM ? "Play" : "Open");
	if (depth > 0)
		x = chip(x, cy, "B", "Back");
	if (t && t->emu >= 0 && emus[t->emu].n_core > 1)
		x = chip(x, cy, "X", "Emulator");
	if (depth > 0)
		x = chip(x, cy, "Y", "Search");
	if (s->n > visible(s))
		x = chip(x, cy, "L/R", "Letter");
	if (depth == 0)
		chip(x, cy, "SELECT+START", "Exit to TrimUI");
	if (s->n) {
		snprintf(right, sizeof right, "%d / %d", s->sel + 1, s->n);
		draw_text(&cv, &font_sm, cv.w - 36 - text_width(&font_sm, right), text_y(&font_sm, cy), right, C_DIM);
	}
}

static char initial(const char *s)
{
	for (; *s; s++) {
		if (*s >= 'a' && *s <= 'z')
			return *s - 32;
		if ((*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9'))
			return *s;
	}
	return '?';
}

/* Systems show the live game count and emulator, so a picker change shows at once. */
static const char *item_sub(const item_t *t, char *buf, size_t sz)
{
	if (t->kind != IT_SYSTEM)
		return t->sub;
	const emu_t *e = &emus[t->emu];
	int c = lib_core_index(e);
	int n = snprintf(buf, sz, "%s - %d %s", e->code, e->count, e->count == 1 ? "game" : "games");
	if (e->n_core > 1 && n > 0 && (size_t)n < sz)
		snprintf(buf + n, sz - n, " - %s", c >= 0 ? e->core[c].name : e->launch);
	return buf;
}

static void render_list(const screen_t *s)
{
	int vis = visible(s), compact = s->row_h < ROW_H, bar = s->n > vis;
	int x = 32, w = cv.w - 64 - (bar ? 18 : 0);
	char buf[160], subbuf[160];

	if (!s->n) {
		const char *msg = s->src ? (query[0] ? "No matches" : "Type to search") : "Nothing here";
		draw_text(&cv, &font_mid, (cv.w - text_width(&font_mid, msg)) / 2,
			  text_y(&font_mid, (LIST_Y + (kb_open ? KB_Y : FOOTER_Y)) / 2), msg, C_DIM);
		return;
	}
	for (int i = 0; i < vis && s->top + i < s->n; i++) {
		const item_t *t = &s->it[s->top + i];
		const char *sub = item_sub(t, subbuf, sizeof subbuf);
		int on = s->top + i == s->sel, y = LIST_Y + i * s->row_h, h = s->row_h - 8;
		int tx = x + 24;

		fill_rrect(&cv, x, y, w, h, compact ? 10 : 14, on ? C_ACCENT : C_ROW);
		if (!compact) {
			int bs = 44, bx = x + 11, by = y + (h - bs) / 2;
			char ini[2] = { initial(t->label), 0 };
			fill_rrect(&cv, bx, by, bs, bs, 11, on ? 0x2E000000 : 0x12FFFFFF);
			draw_text(&cv, &font_mid, bx + (bs - text_width(&font_mid, ini)) / 2,
				  text_y(&font_mid, by + bs / 2), ini, on ? 0xFFFFFFFF : C_TEXT);
			tx = bx + bs + 18;
		}
		int tw = x + w - tx - 48;
		fit_text(&font_mid, t->label, tw, buf, sizeof buf);
		if (sub && !compact) {
			draw_text(&cv, &font_mid, tx, text_y(&font_mid, y + 22), buf, on ? 0xFFFFFFFF : C_TEXT);
			fit_text(&font_sm, sub, tw, buf, sizeof buf);
			draw_text(&cv, &font_sm, tx, text_y(&font_sm, y + 47), buf, on ? 0xD0FFFFFF : C_DIM);
		} else {
			draw_text(&cv, &font_mid, tx, text_y(&font_mid, y + h / 2), buf, on ? 0xFFFFFFFF : C_TEXT);
		}
		if (on)
			draw_text(&cv, &font_mid, x + w - 36, text_y(&font_mid, y + h / 2), ">", 0xFFFFFFFF);
	}
	if (bar) {
		int sx = cv.w - 32 - 6, th = vis * s->row_h - 8;
		int thumb = th * vis / s->n < 24 ? 24 : th * vis / s->n;
		fill_rrect(&cv, sx, LIST_Y, 6, th, 3, 0x12FFFFFF);
		fill_rrect(&cv, sx, LIST_Y + (th - thumb) * s->top / (s->n - vis), 6, thumb, 3, 0x60FFFFFF);
	}
}

static void render_info(void)
{
	char rows[12][2][96], buf[96];
	int n = info_rows(rows);

	for (int i = 0; i < n; i++) {
		int cy = LIST_Y + 22 + i * 54;
		if (i % 2 == 0)
			fill_rrect(&cv, 32, cy - 25, cv.w - 64, 50, 12, C_ROW);
		draw_text(&cv, &font_sm, 56, text_y(&font_sm, cy), rows[i][0], C_DIM);
		fit_text(&font_mid, rows[i][1], cv.w - 300 - 56, buf, sizeof buf);
		draw_text(&cv, &font_mid, 300, text_y(&font_mid, cy), buf, C_TEXT);
	}
}

static void render_dialog(const char *msg, int buttons)
{
	int w = 600, h = buttons ? 220 : 150, x = (cv.w - w) / 2, y = (cv.h - h) / 2;
	char buf[96];

	fill_rect(&cv, 0, 0, cv.w, cv.h, 0xB4000000);
	fill_rrect(&cv, x, y, w, h, 22, 0xFF25272D);
	fit_text(&font_mid, msg, w - 80, buf, sizeof buf);
	draw_text(&cv, &font_mid, x + (w - text_width(&font_mid, buf)) / 2,
		  text_y(&font_mid, y + (buttons ? 76 : h / 2)), buf, C_TEXT);
	if (buttons)
		chip(chip(x + w / 2 - 120, y + h - 58, "A", "Yes"), y + h - 58, "B", "No");
}

static int kb_group(int col)
{
	return col < 4 ? 0 : col < 6 ? 1 : col < 8 ? 2 : 3;
}

static void render_keyboard(void)
{
	int kw = 88, kh = 48, gap = 8, x0 = (cv.w - (10 * kw + 9 * gap)) / 2;
	int fy = KB_Y + 14, fh = 52;
	char buf[64];

	fill_rect(&cv, 0, KB_Y, cv.w, cv.h - KB_Y, 0xF5181A1F);
	fill_rect(&cv, 0, KB_Y, cv.w, 1, C_LINE);

	fill_rrect(&cv, x0, fy, cv.w - 2 * x0, fh, 12, 0x14FFFFFF);
	int tx = draw_text(&cv, &font_mid, x0 + 18, text_y(&font_mid, fy + fh / 2), query, C_TEXT);
	fill_rect(&cv, tx + 3, fy + 12, 3, fh - 24, C_ACCENT);
	if (query[0])
		snprintf(buf, sizeof buf, "%d found", cur()->n);
	else
		snprintf(buf, sizeof buf, "A type  B delete  START done");
	draw_text(&cv, &font_sm, cv.w - x0 - 18 - text_width(&font_sm, buf), text_y(&font_sm, fy + fh / 2), buf, C_DIM);

	for (int r = 0; r < 5; r++) {
		int y = fy + fh + 14 + r * (kh + gap);
		for (int c = 0; c < 10; c++) {
			const font_t *f = &font_mid;
			char k[2] = { 0, 0 };
			const char *label = k;
			int on = r == kb_row && c == kb_col, w = kw;
			if (r < 4) {
				k[0] = kb_keys[r][c];
			} else {
				int g = kb_group(c);
				if (c != kb_wide[g].col)
					continue;
				on = r == kb_row && kb_group(kb_col) == g;
				w = kb_wide[g].span * kw + (kb_wide[g].span - 1) * gap;
				label = kb_wide[g].label;
				f = &font_sm;
			}
			int x = x0 + c * (kw + gap);
			fill_rrect(&cv, x, y, w, kh, 10, on ? C_ACCENT : r < 4 ? 0x14FFFFFF : 0x22FFFFFF);
			draw_text(&cv, f, x + (w - text_width(f, label)) / 2, text_y(f, y + kh / 2), label,
				  on ? 0xFFFFFFFF : C_TEXT);
		}
	}
}

static void render_picker(void)
{
	const emu_t *e = &emus[picker];
	int rh = 56, w = 640, h = 110 + e->n_core * rh + 66, x = (cv.w - w) / 2, y = (cv.h - h) / 2;
	int now = lib_core_index(e);
	char title[96], buf[96];

	fill_rect(&cv, 0, 0, cv.w, cv.h, 0xB4000000);
	fill_rrect(&cv, x, y, w, h, 22, 0xFF25272D);
	snprintf(title, sizeof title, "Emulator for %s", e->name);
	fit_text(&font_mid, title, w - 64, buf, sizeof buf);
	draw_text(&cv, &font_mid, x + (w - text_width(&font_mid, buf)) / 2, text_y(&font_mid, y + 50), buf, C_TEXT);
	for (int i = 0; i < e->n_core; i++) {
		int ry = y + 90 + i * rh, on = i == picker_sel, cy = ry + (rh - 8) / 2;
		fill_rrect(&cv, x + 24, ry, w - 48, rh - 8, 12, on ? C_ACCENT : C_ROW);
		draw_text(&cv, &font_mid, x + 48, text_y(&font_mid, cy), e->core[i].name, on ? 0xFFFFFFFF : C_TEXT);
		if (i == now)
			draw_text(&cv, &font_sm, x + w - 48 - text_width(&font_sm, "current"), text_y(&font_sm, cy),
				  "current", on ? 0xD0FFFFFF : C_DIM);
	}
	chip(chip(x + w / 2 - 130, y + h - 44, "A", "Use"), y + h - 44, "B", "Cancel");
}

static void render(void)
{
	const screen_t *s = cur();

	vgradient(&cv, C_BG_TOP, C_BG_BOT);
	render_header(depth > 0 ? s->title : NULL);
	if (s->kind == SCR_INFO)
		render_info();
	else
		render_list(s);
	if (kb_open)
		render_keyboard();
	else
		render_footer(s);
	if (picker >= 0)
		render_picker();
	if (confirm == IT_REBOOT)
		render_dialog("Reboot the device?", 1);
	else if (confirm == IT_POWEROFF)
		render_dialog("Power off the device?", 1);
}

static void show_busy(const char *msg)
{
	render();
	render_dialog(msg, 0);
	fb_present(&cv);
}

/* Stock premainui.sh clears these before MainUI returns; the N64 launch scripts set
 * input_dpad_to_joystick and never undo it, which would leave kiln without a d-pad. */
static void reset_inputd(void)
{
	unlink("/tmp/trimui_inputd/input_no_dpad");
	unlink("/tmp/trimui_inputd/input_dpad_to_joystick");
}

/* Run a stock launch script from dir, the way MainUI does, and wait for it. Emulators
 * get exactly MainUI's environment. Apps and ports also get what `dev.sh portmaster`
 * adds for PortMaster: bash (its launcher and port scripts need it; stock app scripts are
 * plain sh and run the same under it), kiln/bin (python3) and, ahead of the stock
 * libraries, kiln/lib (libpng16, and newer zlib and SDL2_mixer than stock's; see
 * tools/compat-libs.sh). */
static void run(const char *label, const char *dir, const char *script, const char *arg,
		int app)
{
	const char *shell = app && !access("/bin/bash", X_OK) ? "/bin/bash" : "/bin/sh";
	char msg[96];
	int st = 0;

	snprintf(msg, sizeof msg, "Starting %s...", label);
	show_busy(msg);
	fprintf(stderr, "launch: %s/%s %s\n", dir, script, arg ? arg : "");

	pid_t pid = fork();
	if (pid == 0) {
		setpgid(0, 0); /* own group, so quitting kiln can stop the whole app */
		if (chdir(dir))
			_exit(127);
		setenv("LD_LIBRARY_PATH", app ? KILN_LIB ":/usr/trimui/lib" : "/usr/trimui/lib", 1);
		setenv("PATH", app ? KILN_BIN ":" STOCK_PATH : STOCK_PATH, 1);
		setenv("HOME", "/", 1);
		execl(shell, strrchr(shell, '/') + 1, script, arg, (char *)NULL);
		_exit(127);
	}
	if (pid > 0) {
		setpgid(pid, pid);
		/* Told to quit mid-game: take the app (launch.sh and its emulator) down with us. */
		while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
			if (quit)
				kill(-pid, SIGTERM);
		fprintf(stderr, "launch: exited, status %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
	}
	reset_inputd();
	fb_restore();
	input_flush();
}

typedef struct {
	screen_t *s;
	const char *dir, *sub;
	int emu;
} scan_ctx_t;

static void add_entry(void *ctx, int is_dir, const char *name)
{
	scan_ctx_t *c = ctx;
	char path[1024], label[260];

	snprintf(path, sizeof path, "%s/%s", c->dir, name);
	snprintf(label, sizeof label, is_dir ? "%s/" : "%s", name);
	char *dot = strrchr(label, '.');
	if (!is_dir && dot && dot != label)
		*dot = 0;
	add(c->s, is_dir ? IT_FOLDER : IT_ROM, label, c->sub, path, NULL, c->emu);
}

static void open_dir(int emu, const char *dir, const char *title)
{
	screen_t *s = push(title, SCR_LIST, ROW_H_SM);
	scan_ctx_t c = { s, dir, NULL, emu };

	if (!s)
		return;
	uint64_t t0 = now_ms();
	lib_scan(&emus[emu], dir, add_entry, &c);
	sort_items(s);
	fprintf(stderr, "browse: %s, %d entries in %llu ms\n", dir, s->n,
		(unsigned long long)(now_ms() - t0));
}

static void open_games(void)
{
	if (n_emus < 0) {
		uint64_t t0 = now_ms();
		show_busy("Scanning games...");
		n_emus = lib_load_emus(&emus);
		for (int i = 0; i < n_emus; i++)
			emus[i].count = lib_scan(&emus[i], emus[i].roms, NULL, NULL);
		fprintf(stderr, "games: %d emulators scanned in %llu ms\n", n_emus,
			(unsigned long long)(now_ms() - t0));
	}
	screen_t *s = push("Games", SCR_LIST, ROW_H);
	if (!s)
		return;
	s->global = 1;
	for (int i = 0; i < n_emus; i++)
		if (emus[i].count > 0)
			add(s, IT_SYSTEM, emus[i].name, "", emus[i].roms, NULL, i); /* sub drawn live by item_sub */
	sort_items(s);
}

/* Case-insensitive: does some word of hay start with the first n chars of needle? */
static int has_word(const char *hay, const char *needle, size_t n)
{
	for (const char *p = hay; *p; p++)
		if ((p == hay || !isalnum((unsigned char)p[-1])) && !strncasecmp(p, needle, n))
			return 1;
	return 0;
}

/* Every space-separated word of q starts a word of s, so "son 2" finds "Sonic The Hedgehog 2". */
static int matches(const char *s, const char *q)
{
	while (*q) {
		q += strspn(q, " ");
		size_t n = strcspn(q, " ");
		if (n && !has_word(s, q, n))
			return 0;
		q += n;
	}
	return 1;
}

/* Rebuild the search results screen (the current one) from its source list. */
static void refilter(void)
{
	screen_t *s = cur();

	clear_items(s);
	snprintf(s->title, sizeof s->title, "Search: %s", query);
	if (!query[0])
		return;
	for (int i = 0; i < s->src->n; i++) {
		const item_t *t = &s->src->it[i];
		if (matches(t->label, query))
			add(s, t->kind, t->label, t->sub, t->path, t->cmd, t->emu);
	}
}

static void build_all_games(void)
{
	uint64_t t0 = now_ms();

	show_busy("Indexing games...");
	all_games.row_h = ROW_H;
	for (int i = 0; i < n_emus; i++) {
		scan_ctx_t c = { &all_games, emus[i].roms, emus[i].name, i };
		if (emus[i].count > 0)
			lib_scan(&emus[i], emus[i].roms, add_entry, &c);
	}
	sort_items(&all_games);
	fprintf(stderr, "search: indexed %d games in %llu ms\n", all_games.n,
		(unsigned long long)(now_ms() - t0));
}

/* Y: filter this list, or every system's games from the Games screen. */
static void open_search(void)
{
	screen_t *s = cur();
	const screen_t *src = s;
	int row_h = ROW_H_SM;

	if (s->src) { /* already a results screen: edit the query */
		kb_open = 1;
		return;
	}
	if (s->global) {
		if (!all_games.n)
			build_all_games();
		src = &all_games;
		row_h = ROW_H;
	}
	screen_t *r = push("", SCR_LIST, row_h);
	if (!r)
		return;
	r->src = src;
	query[0] = 0;
	kb_open = 1;
	kb_row = 1;
	kb_col = 0;
	refilter();
}

/* Closing with an empty query backs out of the search entirely. */
static void kb_close(void)
{
	kb_open = 0;
	if (!query[0])
		pop();
}

static void kb_key(int k)
{
	size_t len = strlen(query);
	char before[sizeof query];

	memcpy(before, query, sizeof query);
	switch (k) {
	case K_UP:
		kb_row = (kb_row + 4) % 5;
		break;
	case K_DOWN:
		kb_row = (kb_row + 1) % 5;
		break;
	case K_LEFT:
		kb_col = kb_row == 4 ? kb_wide[(kb_group(kb_col) + 3) % 4].col : (kb_col + 9) % 10;
		break;
	case K_RIGHT:
		kb_col = kb_row == 4 ? kb_wide[(kb_group(kb_col) + 1) % 4].col : (kb_col + 1) % 10;
		break;
	case K_A:
		if (kb_row < 4) {
			if (len < sizeof query - 1)
				query[len] = kb_keys[kb_row][kb_col], query[len + 1] = 0;
			break;
		}
		switch (kb_group(kb_col)) {
		case 0: if (len < sizeof query - 1) query[len] = ' ', query[len + 1] = 0; break;
		case 1: if (len) query[len - 1] = 0; break;
		case 2: query[0] = 0; break;
		case 3: kb_close(); return;
		}
		break;
	case K_X:
		if (len < sizeof query - 1)
			query[len] = ' ', query[len + 1] = 0;
		break;
	case K_B:
		if (!len) {
			kb_close();
			return;
		}
		query[len - 1] = 0;
		break;
	case K_Y:
	case K_START:
		kb_close();
		return;
	}
	if (strcmp(before, query))
		refilter();
}

static void open_picker(const item_t *t)
{
	if (t->emu < 0 || emus[t->emu].n_core < 2)
		return;
	picker = t->emu;
	picker_sel = lib_core_index(&emus[picker]);
	if (picker_sel < 0)
		picker_sel = 0;
}

static void picker_key(int k)
{
	emu_t *e = &emus[picker];

	if (k == K_UP && picker_sel > 0)
		picker_sel--;
	else if (k == K_DOWN && picker_sel < e->n_core - 1)
		picker_sel++;
	else if (k == K_B)
		picker = -1;
	else if (k == K_A) {
		if (lib_set_core(e, picker_sel))
			fprintf(stderr, "picker: could not update %s/config.json\n", e->dir);
		else
			fprintf(stderr, "picker: %s now uses %s\n", e->code, e->launch);
		picker = -1;
	}
}

/* Resume the runtrimui.sh loop that dev.sh froze; it respawns MainUI once we exit. */
static void to_stock(void)
{
	DIR *d = opendir("/proc");
	struct dirent *e;

	while (d && (e = readdir(d))) {
		char path[300], cmd[256];
		if (e->d_name[0] < '0' || e->d_name[0] > '9')
			continue;
		snprintf(path, sizeof path, "/proc/%s/cmdline", e->d_name);
		FILE *f = fopen(path, "re");
		if (!f)
			continue;
		size_t n = fread(cmd, 1, sizeof cmd - 1, f);
		fclose(f);
		for (size_t i = 0; i < n; i++)
			if (!cmd[i])
				cmd[i] = ' ';
		cmd[n] = 0;
		if (strstr(cmd, "runtrimui.sh"))
			kill(atoi(e->d_name), SIGCONT);
	}
	if (d)
		closedir(d);
}

static void do_power(int kind)
{
	const char *cmd = kind == IT_REBOOT ? "reboot" : "poweroff";

	confirm = -1;
	show_busy(kind == IT_REBOOT ? "Rebooting..." : "Powering off...");
	sync();
	execlp(cmd, cmd, (char *)NULL);
	reboot(kind == IT_REBOOT ? RB_AUTOBOOT : RB_POWER_OFF);
}

static void activate(const item_t *t)
{
	screen_t *s;

	switch (t->kind) {
	case IT_GAMES:
		open_games();
		break;
	case IT_APPS:
		if ((s = push("Apps", SCR_LIST, ROW_H)))
			fill_apps(s, APPS_DIR, "App");
		break;
	case IT_PORTS:
		if ((s = push("Ports", SCR_LIST, ROW_H)))
			fill_apps(s, PORTS_DIR, "Port");
		break;
	case IT_SYSTEM:
	case IT_FOLDER:
		open_dir(t->emu, t->path, t->label);
		break;
	case IT_ROM:
		run(t->label, emus[t->emu].dir, emus[t->emu].launch, t->path, 0);
		break;
	case IT_APP:
		run(t->label, t->path, t->cmd, NULL, 1);
		break;
	case IT_INFO:
		push("System", SCR_INFO, ROW_H);
		break;
	case IT_STOCK:
		quit = 2;
		break;
	default:
		confirm = t->kind;
		break;
	}
}

/* L/R: move to the first entry of the previous/next starting letter. */
static void jump_letter(screen_t *s, int dir)
{
	int i = s->sel;
	char c = initial(s->it[i].label);

	if (dir > 0) {
		while (i < s->n - 1 && initial(s->it[i].label) == c)
			i++;
	} else {
		while (i > 0 && initial(s->it[i - 1].label) == c)
			i--;
		if (i == s->sel && i > 0) {
			c = initial(s->it[--i].label);
			while (i > 0 && initial(s->it[i - 1].label) == c)
				i--;
		}
	}
	s->sel = i;
	s->top = i + visible(s) > s->n ? (s->n > visible(s) ? s->n - visible(s) : 0) : i;
}

static void list_key(screen_t *s, int k)
{
	int vis = visible(s);

	if (k == K_B) {
		if (depth > 0)
			pop();
		return;
	}
	if (k == K_Y && depth > 0) {
		open_search();
		return;
	}
	if (!s->n)
		return;
	if (k == K_X) {
		open_picker(&s->it[s->sel]);
		return;
	}
	switch (k) {
	case K_UP:    if (s->sel > 0) s->sel--; break;
	case K_DOWN:  if (s->sel < s->n - 1) s->sel++; break;
	case K_LEFT:  s->sel = s->sel > vis ? s->sel - vis : 0; break;
	case K_RIGHT: s->sel = s->sel + vis < s->n ? s->sel + vis : s->n - 1; break;
	case K_L:     jump_letter(s, -1); break;
	case K_R:     jump_letter(s, 1); break;
	case K_A:     activate(&s->it[s->sel]); break;
	}
	if (s->sel < s->top)
		s->top = s->sel;
	if (s->sel >= s->top + vis)
		s->top = s->sel - vis + 1;
}

static void load_tz(void)
{
	char tz[64];

	if (!getenv("TZ") && read_file("/etc/TZ", tz, sizeof tz) > 0)
		setenv("TZ", tz, 1);
	tzset();
}

int main(int argc, char **argv)
{
	struct sigaction sa = { .sa_handler = on_signal };
	int dirty = 1;

	setvbuf(stderr, NULL, _IOLBF, 0);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	load_tz();

	if (input_open("TRIMUI Player1")) {
		fprintf(stderr, "input: TRIMUI Player1 not found\n");
		return 1;
	}
	/* Escape hatch for autostart: hold B while booting to keep the stock UI. */
	if (argc > 1 && !strcmp(argv[1], "--boot") && input_down_now(BTN_SOUTH)) {
		fprintf(stderr, "kiln: B held at boot, leaving the screen to TrimUI\n");
		return 3;
	}
	if (fb_open(&cv)) {
		fprintf(stderr, "fb0: %s\n", strerror(errno));
		return 1;
	}
	reset_inputd();
	screen_t *root = push("", SCR_LIST, ROW_H);
	add(root, IT_GAMES, "Games", "Browse ROMs on the SD card", NULL, NULL, -1);
	add(root, IT_PORTS, "Ports", "PC game ports, installed with PortMaster", NULL, NULL, -1);
	add(root, IT_APPS, "Apps", "Tools and apps on the SD card", NULL, NULL, -1);
	add(root, IT_INFO, "System info", "Hardware, battery, storage, network", NULL, NULL, -1);
	add(root, IT_STOCK, "Return to TrimUI", "Hand the screen back to the stock launcher", NULL, NULL, -1);
	add(root, IT_REBOOT, "Reboot", "Restart the device", NULL, NULL, -1);
	add(root, IT_POWEROFF, "Power off", "Turn the device off", NULL, NULL, -1);

	while (!quit) {
		if (dirty)
			render(), fb_present(&cv);
		int k = input_next(1000);
		dirty = 1; /* redraw on every key and once a second for the clock */
		if (k == K_NONE)
			continue;
		if ((k == K_START && input_held(K_SELECT)) || (k == K_SELECT && input_held(K_START))) {
			quit = 2;
			break;
		}
		if (confirm >= 0) {
			if (k == K_A)
				do_power(confirm);
			else if (k == K_B)
				confirm = -1;
			continue;
		}
		if (picker >= 0) {
			picker_key(k);
			continue;
		}
		if (kb_open) {
			kb_key(k);
			continue;
		}
		if (k == K_MENU) {
			while (depth > 0)
				pop();
			continue;
		}
		if (cur()->kind == SCR_INFO) {
			if (k == K_B)
				pop();
			continue;
		}
		list_key(cur(), k);
	}
	if (quit == 2)
		to_stock();
	fprintf(stderr, "kiln: exit (%d)\n", (int)quit);
	return 0;
}
