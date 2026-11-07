#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "gfx.h"

static int fb_fd = -1;
static uint8_t *fb_mem;
static struct fb_var_screeninfo vinfo, orig;
static struct fb_fix_screeninfo finfo;
static int npages, page; /* page = the one on screen */

int fb_open(canvas_t *c)
{
	fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (fb_fd < 0)
		return -1;
	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) || ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo))
		return -1;
	if (vinfo.bits_per_pixel != 32) {
		fprintf(stderr, "fb: %u bpp unsupported\n", vinfo.bits_per_pixel);
		return -1;
	}
	orig = vinfo;
	npages = vinfo.yres_virtual >= vinfo.yres * 2 ? 2 : 1;
	fb_mem = mmap(NULL, (size_t)finfo.line_length * vinfo.yres * npages,
		      PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (fb_mem == MAP_FAILED)
		return -1;
	page = vinfo.yoffset >= vinfo.yres;
	ioctl(fb_fd, FBIOBLANK, FB_BLANK_UNBLANK);

	c->w = vinfo.xres;
	c->h = vinfo.yres;
	c->px = calloc((size_t)c->w * c->h, 4);
	fprintf(stderr, "fb: %ux%u virt %ux%u stride %u rgba offs %u/%u/%u/%u pages %d\n",
		vinfo.xres, vinfo.yres, vinfo.xres_virtual, vinfo.yres_virtual, finfo.line_length,
		vinfo.red.offset, vinfo.green.offset, vinfo.blue.offset, vinfo.transp.offset, npages);
	return c->px ? 0 : -1;
}

/* Copy the canvas into the hidden page, then pan to it. */
void fb_present(const canvas_t *c)
{
	int back = npages > 1 ? !page : page;
	uint8_t *dst = fb_mem + (size_t)back * vinfo.yres * finfo.line_length;
	int native = vinfo.red.offset == 16 && vinfo.green.offset == 8 && vinfo.blue.offset == 0;

	for (int y = 0; y < c->h; y++) {
		uint32_t *d = (uint32_t *)(dst + (size_t)y * finfo.line_length);
		const uint32_t *s = c->px + (size_t)y * c->w;
		if (native) {
			memcpy(d, s, (size_t)c->w * 4);
			continue;
		}
		for (int x = 0; x < c->w; x++)
			d[x] = (s[x] >> 16 & 255) << vinfo.red.offset | (s[x] >> 8 & 255) << vinfo.green.offset |
			       (s[x] & 255) << vinfo.blue.offset |
			       (vinfo.transp.length ? 255u << vinfo.transp.offset : 0);
	}
	if (npages == 1)
		return;
	vinfo.xoffset = 0;
	vinfo.yoffset = back * vinfo.yres;
	if (ioctl(fb_fd, FBIOPAN_DISPLAY, &vinfo) == 0) {
		page = back;
	} else {
		perror("fb: pan failed, drawing to the visible page");
		npages = 1;
		fb_present(c);
	}
}

/* An app we launched may have changed the mode; put ours back. */
void fb_restore(void)
{
	struct fb_var_screeninfo cur;

	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &cur))
		return;
	if (cur.xres != orig.xres || cur.yres != orig.yres || cur.yres_virtual != orig.yres_virtual ||
	    cur.bits_per_pixel != orig.bits_per_pixel) {
		fprintf(stderr, "fb: mode changed by app, restoring\n");
		cur = orig;
		cur.yoffset = 0;
		ioctl(fb_fd, FBIOPUT_VSCREENINFO, &cur);
	}
	ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo);
	page = vinfo.yoffset >= vinfo.yres;
}

static inline uint32_t mix(uint32_t d, uint32_t s, unsigned a)
{
	if (a >= 255)
		return s | 0xff000000;
	if (!a)
		return d;
	unsigned ia = 255 - a;
	unsigned r = ((s >> 16 & 255) * a + (d >> 16 & 255) * ia) / 255;
	unsigned g = ((s >> 8 & 255) * a + (d >> 8 & 255) * ia) / 255;
	unsigned b = ((s & 255) * a + (d & 255) * ia) / 255;
	return 0xff000000 | r << 16 | g << 8 | b;
}

void fill_rect(canvas_t *c, int x, int y, int w, int h, uint32_t col)
{
	int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
	int x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;
	unsigned a = col >> 24;

	for (int j = y0; j < y1; j++) {
		uint32_t *row = c->px + (size_t)j * c->w;
		for (int i = x0; i < x1; i++)
			row[i] = mix(row[i], col, a);
	}
}

/* Rounded rectangle with antialiased corners. */
void fill_rrect(canvas_t *c, int x, int y, int w, int h, int r, uint32_t col)
{
	unsigned ca = col >> 24;

	if (r * 2 > w)
		r = w / 2;
	if (r * 2 > h)
		r = h / 2;
	for (int j = 0; j < h; j++) {
		int py = y + j;
		if (py < 0 || py >= c->h)
			continue;
		float fy = j + 0.5f;
		float dy = fy < r ? r - fy : fy > h - r ? fy - (h - r) : 0;
		uint32_t *row = c->px + (size_t)py * c->w;
		for (int i = 0; i < w; i++) {
			int px = x + i;
			if (px < 0 || px >= c->w)
				continue;
			unsigned a = ca;
			if (dy > 0) {
				float fx = i + 0.5f;
				float dx = fx < r ? r - fx : fx > w - r ? fx - (w - r) : 0;
				if (dx > 0) {
					float cov = r - sqrtf(dx * dx + dy * dy) + 0.5f;
					if (cov <= 0)
						continue;
					if (cov < 1)
						a = (unsigned)(a * cov);
				}
			}
			row[px] = mix(row[px], col, a);
		}
	}
}

void vgradient(canvas_t *c, uint32_t top, uint32_t bot)
{
	for (int y = 0; y < c->h; y++) {
		uint32_t col = mix(top, bot, y * 255 / (c->h - 1));
		uint32_t *row = c->px + (size_t)y * c->w;
		for (int x = 0; x < c->w; x++)
			row[x] = col;
	}
}

static const glyph_t *glyph(const font_t *f, unsigned ch)
{
	if (ch < 32 || ch > 126)
		ch = '?';
	return &f->g[ch - 32];
}

/* Step one UTF-8 codepoint; anything outside ASCII renders as '?'. */
static const char *next_ch(const char *s, unsigned *ch)
{
	unsigned char b = *s++;

	*ch = b < 0x80 ? b : '?';
	if (b >= 0x80)
		while (((unsigned char)*s & 0xc0) == 0x80)
			s++;
	return s;
}

int text_width(const font_t *f, const char *s)
{
	int w = 0;
	unsigned ch;

	while (*s) {
		s = next_ch(s, &ch);
		w += glyph(f, ch)->adv;
	}
	return w;
}

/* Returns the pen x after the last glyph. */
int draw_text(canvas_t *c, const font_t *f, int x, int y, const char *s, uint32_t col)
{
	unsigned ca = col >> 24, ch;

	while (*s) {
		s = next_ch(s, &ch);
		const glyph_t *g = glyph(f, ch);
		const uint8_t *src = f->px + g->off;
		for (int j = 0; j < g->h; j++) {
			int py = y + g->y + j;
			if (py < 0 || py >= c->h)
				continue;
			uint32_t *row = c->px + (size_t)py * c->w;
			for (int i = 0; i < g->w; i++) {
				int px = x + g->x + i;
				unsigned a = src[j * g->w + i] * ca / 255;
				if (a && px >= 0 && px < c->w)
					row[px] = mix(row[px], col, a);
			}
		}
		x += g->adv;
	}
	return x;
}

/* Copy s into out, cut with "..." if it is wider than maxw. */
void fit_text(const font_t *f, const char *s, int maxw, char *out, int outsz)
{
	if (text_width(f, s) <= maxw) {
		snprintf(out, outsz, "%s", s);
		return;
	}
	int ell = text_width(f, "..."), w = 0, n = 0;
	unsigned ch;
	while (*s) {
		const char *nx = next_ch(s, &ch);
		int adv = glyph(f, ch)->adv;
		if (w + adv + ell > maxw || n + (nx - s) + 4 > outsz)
			break;
		memcpy(out + n, s, nx - s);
		n += nx - s;
		w += adv;
		s = nx;
	}
	memcpy(out + n, "...", 4);
}
