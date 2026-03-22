#ifndef BITMAPS_H
#define BITMAPS_H

#include "gfx.h"

#define BITMAP_FONT_BASE_WIDTH 5U
#define BITMAP_FONT_BASE_HEIGHT 7U

/* Returns NULL if glyph is not supported. Supports A-Z and 0-9 only. */
const GFX_Bitmap *Bitmap_GetGlyph(char ch);

#endif
