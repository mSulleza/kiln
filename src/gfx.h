#ifndef GFX_H
#define GFX_H

#include <stdint.h>

/* Colors are 0xAARRGGBB; alpha blends onto the canvas. */

typedef struct {
	uint32_t off;
	uint16_t w, h;
	int16_t x, y; /* offset from pen position, y measured from the ascender line */
	uint16_t adv;
} glyph_t;

typedef struct {
	int size, ascent, line_h, cap_top, cap_h;
	const uint8_t *px;
	glyph_t g[95]; /* ASCII 32..126 */
} font_t;

typedef struct {
	int w, h;
	uint32_t *px;
} canvas_t;

int  fb_open(canvas_t *c);
void fb_present(const canvas_t *c);
void fb_restore(void);

void fill_rect(canvas_t *c, int x, int y, int w, int h, uint32_t col);
void fill_rrect(canvas_t *c, int x, int y, int w, int h, int r, uint32_t col);
void vgradient(canvas_t *c, uint32_t top, uint32_t bot);

int  text_width(const font_t *f, const char *s);
int  draw_text(canvas_t *c, const font_t *f, int x, int y, const char *s, uint32_t col);
void fit_text(const font_t *f, const char *s, int maxw, char *out, int outsz);

/* y to pass to draw_text so capital letters are centered on cy. */
static inline int text_y(const font_t *f, int cy) { return cy - f->cap_top - f->cap_h / 2; }

#endif
