#include "gfx.h"
#include "bitmaps.h"

#include <ctype.h>
#include <string.h>
#include <stdbool.h>

#define GFX_CHAR_MAX_BITMAP_BYTES 512U

static size_t gfx_bitmap_required_size(const GFX_Bitmap *bitmap){
    size_t row_bytes = ((size_t)bitmap->width + 7U) / 8U;
    return row_bytes * (size_t)bitmap->height;
}

static uint8_t gfx_bitmap_get_pixel(const GFX_Bitmap *bitmap, uint16_t x, uint16_t y){
    size_t row_bytes = ((size_t)bitmap->width + 7U) / 8U;
    size_t index = (size_t)y * row_bytes + ((size_t)x / 8U);
    uint8_t mask = (uint8_t)(0x80U >> (x % 8U));
    return (bitmap->data[index] & mask) ? 1U : 0U;
}

static void gfx_bitmap_set_pixel(uint8_t *buffer, uint16_t width, uint16_t x, uint16_t y, uint8_t value){
    size_t row_bytes = ((size_t)width + 7U) / 8U;
    size_t index = (size_t)y * row_bytes + ((size_t)x / 8U);
    uint8_t mask = (uint8_t)(0x80U >> (x % 8U));

    if (value != 0U){
        buffer[index] |= mask;
    }
    else{
        buffer[index] &= (uint8_t)(~mask);
    }
}

size_t GFX_CalcBufferSize(uint16_t width, uint16_t height){
    uint16_t pages = (uint16_t)((height + 7U) / 8U);
    return (size_t)width * (size_t)pages;
}

GFX_Framebuffer GFX_Init(uint16_t width, uint16_t height, uint8_t *buffer,
                        size_t buffer_size, GFX_FlushCallback flush_cb, 
                        void *flush_context)
{
    GFX_Framebuffer fb;
    size_t required_size = GFX_CalcBufferSize(width, height);

    fb.width = width;
    fb.height = height;
    fb.pages = (uint16_t)((height + 7U) / 8U);
    fb.stride = width;
    fb.buffer = buffer;
    fb.buffer_size = buffer_size;
    fb.flush_cb = flush_cb;
    fb.flush_context = flush_context;

    if ((buffer == NULL) || (buffer_size < required_size) || 
        (width == 0U) || (height == 0U))
    {
        fb.buffer = NULL;
        fb.buffer_size = 0U;
    }

    return fb;
}

uint8_t GFX_IsReady(const GFX_Framebuffer *fb){
    if (fb == NULL){
        return 0U;
    }

    if ((fb->buffer == NULL) || (fb->width == 0U) || (fb->height == 0U)){
        return 0U;
    }

    return (fb->buffer_size >= GFX_CalcBufferSize(fb->width, fb->height)) ? 1U : 0U;
}

void GFX_Clear(GFX_Framebuffer *fb, uint8_t color){
    if (GFX_IsReady(fb) == 0U){
        return;
    }

    memset(fb->buffer, (color != 0U) ? 0xFF : 0x00, GFX_CalcBufferSize(fb->width, fb->height));
}

void GFX_DrawPixel(GFX_Framebuffer *fb, int16_t x, int16_t y, uint8_t color){
    if (GFX_IsReady(fb) == 0U){
        return;
    }

    if ((x < 0) || (y < 0) || (x >= (int16_t)fb->width) || (y >= (int16_t)fb->height)){
        return;
    }

    uint16_t page = (uint16_t)y / 8U;
    uint16_t index = (uint16_t)x + (uint16_t)(page * fb->stride);
    uint8_t mask = (uint8_t)(1U << ((uint16_t)y % 8U));

    if (color != 0U){
        fb->buffer[index] |= mask;
    }
    else{
        fb->buffer[index] &= (uint8_t)(~mask);
    }
}

void GFX_DrawRectBorder(GFX_Framebuffer *fb, int16_t x, int16_t y, uint16_t width,
                            uint16_t height, uint8_t thickness, uint8_t color)
{
    if ((GFX_IsReady(fb) == 0U) || (width == 0U) || (height == 0U) || (thickness == 0U)){
        return;
    }

    for (uint8_t t = 0U; t < thickness; t++){
        int16_t left = (int16_t)(x + t);
        int16_t top = (int16_t)(y + t);
        int16_t right = (int16_t)(x + (int16_t)width - 1 - (int16_t)t);
        int16_t bottom = (int16_t)(y + (int16_t)height - 1 - (int16_t)t);

        if ((right < left) || (bottom < top)){
            break;
        }

        for (int16_t px = left; px <= right; px++){
            GFX_DrawPixel(fb, px, top, color);
            GFX_DrawPixel(fb, px, bottom, color);
        }

        for (int16_t py = top; py <= bottom; py++){
            GFX_DrawPixel(fb, left, py, color);
            GFX_DrawPixel(fb, right, py, color);
        }
    }
}

void GFX_DrawBitmap(GFX_Framebuffer *fb, const GFX_Bitmap *bitmap, int16_t x, int16_t y){
    if ((GFX_IsReady(fb) == 0U) || (bitmap == NULL) || (bitmap->data == NULL)){
        return;
    }

    if ((bitmap->width == 0U) || (bitmap->height == 0U)){
        return;
    }

    size_t required_size = gfx_bitmap_required_size(bitmap);
    if (bitmap->size < required_size){
        return;
    }

    size_t row_bytes = ((size_t)bitmap->width + 7U) / 8U;

    for (uint16_t by = 0U; by < bitmap->height; by++){
        for (uint16_t bx = 0U; bx < bitmap->width; bx++){
            size_t byte_index = (size_t)by * row_bytes + ((size_t)bx / 8U);
            uint8_t bit_mask = (uint8_t)(0x80U >> (bx % 8U));
            uint8_t pixel_on = (bitmap->data[byte_index] & bit_mask) ? 1U : 0U;

            GFX_DrawPixel(fb, (int16_t)(x + (int16_t)bx), (int16_t)(y + (int16_t)by), pixel_on);
        }
    }
}

void GFX_DrawLine(GFX_Framebuffer *fb, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t color){
    if (GFX_IsReady(fb) == 0U){
        return;
    }

    int16_t dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int16_t sx = (x0 < x1) ? 1 : -1;
    int16_t dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int16_t sy = (y0 < y1) ? 1 : -1;
    int16_t err = (dx > dy ? dx : -dy) / 2;

    while (true) {
        GFX_DrawPixel(fb, x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int16_t e2 = err;
        if (e2 > -dx) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dy) {
            err += dx;
            y0 += sy;
        }
    }
}

void GFX_DrawCircle(GFX_Framebuffer *fb, int16_t cx, int16_t cy, uint16_t radius, uint8_t thickness, uint8_t filled, uint8_t color){
    if ((GFX_IsReady(fb) == 0U) || (radius == 0U)){
        return;
    }
    if (thickness == 0U){
        thickness = 1U;
    }

    for (uint8_t t = 0; t < thickness; ++t) {
        int16_t r = (int16_t)radius - (int16_t)t;
        int16_t x = r;
        int16_t y = 0;
        int16_t err = 1 - x;

        while (x >= y) {
            if (filled) {
                GFX_DrawLine(fb, cx - x, cy + y, cx + x, cy + y, color);
                GFX_DrawLine(fb, cx - y, cy + x, cx + y, cy + x, color);
                GFX_DrawLine(fb, cx - x, cy - y, cx + x, cy - y, color);
                GFX_DrawLine(fb, cx - y, cy - x, cx + y, cy - x, color);
            } else {
                GFX_DrawPixel(fb, cx + x, cy + y, color);
                GFX_DrawPixel(fb, cx + y, cy + x, color);
                GFX_DrawPixel(fb, cx - y, cy + x, color);
                GFX_DrawPixel(fb, cx - x, cy + y, color);
                GFX_DrawPixel(fb, cx - x, cy - y, color);
                GFX_DrawPixel(fb, cx - y, cy - x, color);
                GFX_DrawPixel(fb, cx + y, cy - x, color);
                GFX_DrawPixel(fb, cx + x, cy - y, color);
            }

            y++;
            if (err < 0) {
                err += 2 * y + 1;
            } else {
                x--;
                err += 2 * (y - x + 1);
            }
        }
    }
}

int32_t GFX_ResizeBitmapKeepAspect(const GFX_Bitmap *src, uint16_t target_height,
                    uint8_t *dst_buffer, size_t dst_buffer_size, GFX_Bitmap *out_bitmap)
{
    if ((src == NULL) || (src->data == NULL) || (dst_buffer == NULL) || (out_bitmap == NULL)){
        return -1;
    }

    if ((src->width == 0U) || (src->height == 0U) || (target_height == 0U)){
        return -1;
    }

    if (src->size < gfx_bitmap_required_size(src)){
        return -1;
    }

    uint16_t target_width = (uint16_t)(((uint32_t)src->width * (uint32_t)target_height + ((uint32_t)src->height / 2U)) / (uint32_t)src->height);
    if (target_width == 0U){
        target_width = 1U;
    }

    size_t row_bytes = ((size_t)target_width + 7U) / 8U;
    size_t required_size = row_bytes * (size_t)target_height;
    if (dst_buffer_size < required_size){
        return -1;
    }

    memset(dst_buffer, 0, required_size);

    for (uint16_t y = 0U; y < target_height; y++){
        uint16_t src_y = (uint16_t)(((uint32_t)y * (uint32_t)src->height) / (uint32_t)target_height);
        if (src_y >= src->height){
            src_y = (uint16_t)(src->height - 1U);
        }

        for (uint16_t x = 0U; x < target_width; x++){
            uint16_t src_x = (uint16_t)(((uint32_t)x * (uint32_t)src->width) / (uint32_t)target_width);
            if (src_x >= src->width){
                src_x = (uint16_t)(src->width - 1U);
            }

            gfx_bitmap_set_pixel(dst_buffer, target_width, x, y, gfx_bitmap_get_pixel(src, src_x, src_y));
        }
    }

    out_bitmap->data = dst_buffer;
    out_bitmap->size = required_size;
    out_bitmap->width = target_width;
    out_bitmap->height = target_height;
    return 0;
}

int32_t GFX_DrawChar(GFX_Framebuffer *fb, char ch, int16_t x, int16_t y, uint16_t target_height, uint8_t color){
    const GFX_Bitmap *glyph = Bitmap_GetGlyph(ch);
    GFX_Bitmap src;
    GFX_Bitmap draw_bitmap;
    uint8_t scaled_buffer[GFX_CHAR_MAX_BITMAP_BYTES];

    if ((GFX_IsReady(fb) == 0U) || (glyph == NULL)){
        return -1;
    }

    src = *glyph;
    draw_bitmap = src;

    if ((target_height != 0U) && (target_height != src.height)){
        if (GFX_ResizeBitmapKeepAspect(&src, target_height, scaled_buffer, sizeof(scaled_buffer), &draw_bitmap) != 0){
            return -1;
        }
    }

    for (uint16_t py = 0U; py < draw_bitmap.height; py++){
        for (uint16_t px = 0U; px < draw_bitmap.width; px++){
            if (gfx_bitmap_get_pixel(&draw_bitmap, px, py) != 0U){
                GFX_DrawPixel(fb, (int16_t)(x + (int16_t)px), (int16_t)(y + (int16_t)py), color);
            }
        }
    }

    return 0;
}


int32_t GFX_DrawStr(GFX_Framebuffer *fb, char *str, int16_t x, int16_t y, uint16_t target_height, uint8_t color){
    if (GFX_IsReady(fb) == 0U){
        return -1;
    }
    unsigned int len = strlen(str);
    int16_t cursor_x = x;
    int16_t cursor_y = y;
    int16_t start_x = x;
    uint16_t line_height = (target_height == 0U) ? BITMAP_FONT_BASE_HEIGHT : target_height;
    uint16_t max_line_height = line_height;
    uint16_t padding = (uint16_t)((line_height / 2U) + 1U);

    for (unsigned int i = 0; i < len; i++){
        char ch = str[i];

        if (ch == '\n'){
            cursor_x = start_x;
            cursor_y = (int16_t)(cursor_y + max_line_height + padding);
            max_line_height = line_height;
            continue;
        }

        if (ch == ' '){
            int space_width = (line_height == 0U) ? (int)BITMAP_FONT_BASE_WIDTH
                                                  : (int)(((uint32_t)BITMAP_FONT_BASE_WIDTH * (uint32_t)line_height +
                                                           ((uint32_t)BITMAP_FONT_BASE_HEIGHT / 2U)) /
                                                          (uint32_t)BITMAP_FONT_BASE_HEIGHT);
            cursor_x = (int16_t)(cursor_x + space_width + (space_width / 3));
            continue;
        }

        char up = (char)toupper((unsigned char)ch);
        const GFX_Bitmap *glyph = Bitmap_GetGlyph(up);
        GFX_Bitmap src;
        GFX_Bitmap draw_bitmap;
        uint8_t scaled_buffer[GFX_CHAR_MAX_BITMAP_BYTES];
        if(glyph == NULL){
            int space_width = (line_height == 0U) ? (int)BITMAP_FONT_BASE_WIDTH
                                                  : (int)(((uint32_t)BITMAP_FONT_BASE_WIDTH * (uint32_t)line_height +
                                                           ((uint32_t)BITMAP_FONT_BASE_HEIGHT / 2U)) /
                                                          (uint32_t)BITMAP_FONT_BASE_HEIGHT);
            cursor_x = (int16_t)(cursor_x + space_width + (space_width / 3));
            continue;
        }

        src = *glyph;
        draw_bitmap = src;

        if ((target_height != 0U) && (target_height != src.height)){
            if (GFX_ResizeBitmapKeepAspect(&src, target_height, scaled_buffer, sizeof(scaled_buffer), &draw_bitmap) != 0){
                return -1;
            }
        }

        for (uint16_t py = 0U; py < draw_bitmap.height; py++){
            for (uint16_t px = 0U; px < draw_bitmap.width; px++){
                if (gfx_bitmap_get_pixel(&draw_bitmap, px, py) != 0U){
                    GFX_DrawPixel(fb, (int16_t)(cursor_x + (int16_t)px), (int16_t)(cursor_y + (int16_t)py), color);
                }
            }
        }

        if (draw_bitmap.height > max_line_height){
            max_line_height = draw_bitmap.height;
        }

        cursor_x = (int16_t)(cursor_x + draw_bitmap.width + (draw_bitmap.width / 3));
    }

    return 0;
}

int32_t GFX_Present(const GFX_Framebuffer *fb){
    if ((GFX_IsReady(fb) == 0U) || (fb->flush_cb == NULL)){
        return -1;
    }

    return fb->flush_cb(fb->flush_context,
                        fb->buffer,
                        GFX_CalcBufferSize(fb->width, fb->height),
                        fb->width,
                        fb->height);
}
