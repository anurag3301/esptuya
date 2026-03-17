#ifndef SSD1306_H
#define SSD1306_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OLED_OK 0
#define OLED_ERR_INVALID_ARG (-1)
#define OLED_ERR_IO (-2)

typedef enum{
    OLED_BUS_I2C = 0,
    OLED_BUS_SPI = 1
} OLED_BusType;

typedef int32_t (*OLED_SendI2CFn)(void *user_context,
                                  uint8_t i2c_address_7bit,
                                  const uint8_t *data,
                                  size_t length);

/*
 * SPI callback:
 * - is_data = 0: command bytes
 * - is_data = 1: display data bytes
 */
typedef int32_t (*OLED_SendSPIFn)(void *user_context,
                                  uint8_t is_data,
                                  const uint8_t *data,
                                  size_t length);

typedef struct{
    OLED_BusType bus_type;
    uint16_t width;
    uint16_t height;
    void *user_context;
    union{
        struct{
            uint8_t i2c_address_7bit;
            OLED_SendI2CFn send_fn;
        } i2c;

        struct{
            OLED_SendSPIFn send_fn;
        } spi;
    } transport;
} OLED_Config;

size_t OLED_BufferSize(const OLED_Config *cfg);

int32_t OLED_Init(const OLED_Config *cfg);
int32_t OLED_Clear(const OLED_Config *cfg);
int32_t OLED_Fill(const OLED_Config *cfg, uint8_t pattern);

/*
 * Bitmap format:
 * - Exactly OLED_BufferSize(cfg) bytes
 * - Page-oriented (page count = height/8)
 * - Each byte is a vertical column of 8 pixels in a page (LSB at top)
 */
int32_t OLED_DrawBitmap(const OLED_Config *cfg, const uint8_t *bitmap, size_t length);

/*
 * GFX flush callback adapter.
 * context must be a valid OLED_Config*.
 */
int32_t OLED_GfxFlushCallback(void *context,
                              const uint8_t *buffer,
                              size_t buffer_size,
                              uint16_t width,
                              uint16_t height);

#ifdef __cplusplus
}
#endif

#endif
