#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "input.h"
#include "util.h"

#define REPEAT_DELAY 320
#define REPEAT_RATE  70

static int fd = -1;
static int held[K_COUNT];
static int rep_key = K_NONE;
static uint64_t rep_at;
static int queue[16], qhead, qlen;
static char last[48] = "none yet";

/* Face buttons as reported by trimui_inputd on the Brick (checked on hardware):
 * A=EAST, B=SOUTH, X=WEST, Y=NORTH. */
static int map_key(int code)
{
	switch (code) {
	case BTN_EAST:     return K_A;
	case BTN_SOUTH:    return K_B;
	case BTN_WEST:     return K_X;
	case BTN_NORTH:    return K_Y;
	case BTN_TL:       return K_L;
	case BTN_TR:       return K_R;
	case BTN_SELECT:   return K_SELECT;
	case BTN_START:    return K_START;
	case BTN_MODE:
	case KEY_HOMEPAGE: return K_MENU;
	default:           return K_NONE;
	}
}

static const char *key_name(int code)
{
	switch (code) {
	case BTN_SOUTH:      return "BTN_SOUTH";
	case BTN_EAST:       return "BTN_EAST";
	case BTN_NORTH:      return "BTN_NORTH";
	case BTN_WEST:       return "BTN_WEST";
	case BTN_TL:         return "BTN_TL";
	case BTN_TR:         return "BTN_TR";
	case BTN_SELECT:     return "BTN_SELECT";
	case BTN_START:      return "BTN_START";
	case BTN_MODE:       return "BTN_MODE";
	case BTN_THUMBL:     return "BTN_THUMBL";
	case BTN_THUMBR:     return "BTN_THUMBR";
	case KEY_HOMEPAGE:   return "KEY_HOMEPAGE";
	case KEY_VOLUMEUP:   return "KEY_VOLUMEUP";
	case KEY_VOLUMEDOWN: return "KEY_VOLUMEDOWN";
	case KEY_F1:         return "KEY_F1";
	case KEY_F2:         return "KEY_F2";
	default:             return NULL;
	}
}

int input_open(const char *name)
{
	char path[32], got[64];

	for (int i = 0; i < 16; i++) {
		snprintf(path, sizeof path, "/dev/input/event%d", i);
		int f = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (f < 0)
			continue;
		memset(got, 0, sizeof got);
		if (ioctl(f, EVIOCGNAME(sizeof got - 1), got) >= 0 && !strcmp(got, name)) {
			fprintf(stderr, "input: %s is \"%s\"\n", path, name);
			fd = f;
			return 0;
		}
		close(f);
	}
	return -1;
}

static void set_key(int k, int down)
{
	if (down && !held[k]) {
		if (qlen < 16)
			queue[(qhead + qlen++) % 16] = k;
		if (k <= K_RIGHT) {
			rep_key = k;
			rep_at = now_ms() + REPEAT_DELAY;
		}
	}
	if (!down && rep_key == k)
		rep_key = K_NONE;
	held[k] = down;
}

static void read_events(void)
{
	struct input_event ev[32];
	ssize_t n;

	while ((n = read(fd, ev, sizeof ev)) > 0) {
		for (size_t i = 0; i < (size_t)n / sizeof *ev; i++) {
			const struct input_event *e = &ev[i];
			if (e->type == EV_KEY && e->value != 2) {
				const char *nm = key_name(e->code);
				char buf[48];
				if (nm)
					snprintf(buf, sizeof buf, "%s", nm);
				else
					snprintf(buf, sizeof buf, "key %d", e->code);
				fprintf(stderr, "input: %s %s\n", buf, e->value ? "down" : "up");
				if (e->value)
					snprintf(last, sizeof last, "%s", buf);
				int k = map_key(e->code);
				if (k != K_NONE)
					set_key(k, e->value);
			} else if (e->type == EV_ABS) {
				fprintf(stderr, "input: abs %d = %d\n", e->code, e->value);
				if (e->code == ABS_HAT0Y) {
					set_key(K_UP, e->value < 0);
					set_key(K_DOWN, e->value > 0);
				} else if (e->code == ABS_HAT0X) {
					set_key(K_LEFT, e->value < 0);
					set_key(K_RIGHT, e->value > 0);
				} else {
					continue;
				}
				if (e->value)
					snprintf(last, sizeof last, "%s %d",
						 e->code == ABS_HAT0X ? "HAT0X" : "HAT0Y", e->value);
			}
		}
	}
}

int input_next(int timeout_ms)
{
	uint64_t deadline = timeout_ms < 0 ? UINT64_MAX : now_ms() + timeout_ms;

	for (;;) {
		if (qlen) {
			int k = queue[qhead];
			qhead = (qhead + 1) % 16;
			qlen--;
			return k;
		}
		uint64_t t = now_ms();
		if (rep_key != K_NONE && t >= rep_at) {
			rep_at = t + REPEAT_RATE;
			return rep_key;
		}
		if (t >= deadline)
			return K_NONE;
		uint64_t until = rep_key != K_NONE && rep_at < deadline ? rep_at : deadline;
		struct pollfd p = { .fd = fd, .events = POLLIN };
		int r = poll(&p, 1, until == UINT64_MAX ? -1 : (int)(until - t));
		if (r < 0)
			return K_NONE; /* signal: let the caller check its flags */
		if (r > 0)
			read_events();
	}
}

int input_held(int k)
{
	return held[k];
}

int input_down_now(int code)
{
	unsigned char keys[KEY_MAX / 8 + 1] = { 0 };

	if (ioctl(fd, EVIOCGKEY(sizeof keys), keys) < 0)
		return 0;
	return keys[code / 8] >> (code % 8) & 1;
}

void input_flush(void)
{
	read_events();
	qlen = 0;
	rep_key = K_NONE;
	memset(held, 0, sizeof held);
}

const char *input_last(void)
{
	return last;
}
