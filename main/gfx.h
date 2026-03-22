#ifndef GFX_H
#define GFX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t (*GFX_FlushCallback)(void *context,
                                     const uint8_t *buffer,
                                     size_t buffer_size,
                                     uint16_t width,
                                     uint16_t height);

typedef struct{
    const uint8_t *data;
    size_t size;
    uint16_t width;
    uint16_t height;
} GFX_Bitmap;

typedef struct{
    uint16_t width;
    uint16_t height;
    uint16_t pages;
    uint16_t stride;
    uint8_t *buffer;
    size_t buffer_size;
    GFX_FlushCallback flush_cb;
    void *flush_context;
} GFX_Framebuffer;

size_t GFX_CalcBufferSize(uint16_t width, uint16_t height);

GFX_Framebuffer GFX_Init(uint16_t width, uint16_t height, uint8_t *buffer,
                        size_t buffer_size, GFX_FlushCallback flush_cb, 
                        void *flush_context);

uint8_t GFX_IsReady(const GFX_Framebuffer *fb);
void GFX_Clear(GFX_Framebuffer *fb, uint8_t color);
void GFX_DrawPixel(GFX_Framebuffer *fb, int16_t x, int16_t y, uint8_t color);

/* Draw only border. thickness=1 is a single-pixel border. */
void GFX_DrawRectBorder(GFX_Framebuffer *fb, int16_t x, int16_t y, uint16_t width,
                            uint16_t height, uint8_t thickness, uint8_t color);

/*
 * Bitmap format:
 * - 1 bit per pixel
 * - Row-major
 * - Each row is packed MSB-first in bytes
 */
void GFX_DrawBitmap(GFX_Framebuffer *fb, const GFX_Bitmap *bitmap, int16_t x, int16_t y);

/*
 * Resizes 1bpp row-major bitmap with nearest-neighbor while preserving aspect ratio.
 * target_height defines output size; output width is computed automatically.
 * out_bitmap points to dst_buffer on success.
 */
int32_t GFX_ResizeBitmapKeepAspect(const GFX_Bitmap *src, uint16_t target_height,
                    uint8_t *dst_buffer, size_t dst_buffer_size, GFX_Bitmap *out_bitmap);

/*
 * Draw a single glyph (A-Z, 0-9) using the font bitmap set from bitmaps.h.
 * target_height == 0 draws at native font size.
 */
int32_t GFX_DrawChar(GFX_Framebuffer *fb, char ch, int16_t x, int16_t y, uint16_t target_height, uint8_t color);

int32_t GFX_DrawStr(GFX_Framebuffer *fb, char *str, int16_t x, int16_t y, uint16_t target_height, uint8_t color);

int32_t GFX_Present(const GFX_Framebuffer *fb);

#ifdef __cplusplus
}
#endif

#endif
