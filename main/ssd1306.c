#include "ssd1306.h"

#include <string.h>

#define OLED_CONTROL_CMD 0x00U
#define OLED_CONTROL_DATA 0x40U
#define OLED_DATA_CHUNK_SIZE 16U

static uint8_t oled_is_valid_config(const OLED_Config *cfg){
    if (cfg == NULL){
        return 0U;
    }

    if ((cfg->width == 0U) || (cfg->height == 0U) || 
        ((cfg->height % 8U) != 0U))
    {
        return 0U;
    }

    if (cfg->bus_type == OLED_BUS_I2C){
        return (cfg->transport.i2c.send_fn != NULL) ? 1U : 0U;
    }

    if (cfg->bus_type == OLED_BUS_SPI){
        return (cfg->transport.spi.send_fn != NULL) ? 1U : 0U;
    }

    return 0U;
}

size_t OLED_BufferSize(const OLED_Config *cfg){
    if (cfg == NULL){
        return 0U;
    }

    return (size_t)cfg->width * (size_t)(cfg->height / 8U);
}

static int32_t oled_send_i2c(const OLED_Config *cfg, const uint8_t *data, size_t length){
    return cfg->transport.i2c.send_fn(cfg->user_context,
                                      cfg->transport.i2c.i2c_address_7bit,
                                      data, length);
}

static int32_t oled_send_spi(const OLED_Config *cfg, uint8_t is_data, const uint8_t *data, size_t length){
    return cfg->transport.spi.send_fn(cfg->user_context, is_data, data, length);
}

static int32_t oled_write_command(const OLED_Config *cfg, uint8_t cmd){
    if (cfg->bus_type == OLED_BUS_I2C){
        uint8_t frame[2] = {OLED_CONTROL_CMD, cmd};
        return (oled_send_i2c(cfg, frame, sizeof(frame)) == OLED_OK) ? OLED_OK : OLED_ERR_IO;
    }

    if (cfg->bus_type == OLED_BUS_SPI){
        return (oled_send_spi(cfg, 0U, &cmd, 1U) == OLED_OK) ? OLED_OK : OLED_ERR_IO;
    }

    return OLED_ERR_INVALID_ARG;
}

static int32_t oled_write_data(const OLED_Config *cfg, const uint8_t *data, size_t length){
    uint8_t frame[1 + OLED_DATA_CHUNK_SIZE];

    if ((data == NULL) || (length == 0U)){
        return OLED_ERR_INVALID_ARG;
    }

    if (cfg->bus_type == OLED_BUS_I2C){
        frame[0] = OLED_CONTROL_DATA;

        while (length > 0U){
            size_t chunk = (length > OLED_DATA_CHUNK_SIZE) ? OLED_DATA_CHUNK_SIZE : length;
            memcpy(&frame[1], data, chunk);

            if (oled_send_i2c(cfg, frame, chunk + 1U) != OLED_OK){
                return OLED_ERR_IO;
            }

            data += chunk;
            length -= chunk;
        }

        return OLED_OK;
    }

    if (cfg->bus_type == OLED_BUS_SPI){
        while (length > 0U){
            size_t chunk = (length > OLED_DATA_CHUNK_SIZE) ? OLED_DATA_CHUNK_SIZE : length;
            if (oled_send_spi(cfg, 1U, data, chunk) != OLED_OK){
                return OLED_ERR_IO;
            }

            data += chunk;
            length -= chunk;
        }

        return OLED_OK;
    }

    return OLED_ERR_INVALID_ARG;
}

static int32_t oled_set_full_window(const OLED_Config *cfg)
{
    uint8_t pages = (uint8_t)(cfg->height / 8U);

    if (oled_write_command(cfg, 0x21U) != OLED_OK){ /* Set column address */
        return OLED_ERR_IO;
    }
    if (oled_write_command(cfg, 0x00U) != OLED_OK){
        return OLED_ERR_IO;
    }
    if (oled_write_command(cfg, (uint8_t)(cfg->width - 1U)) != OLED_OK){
        return OLED_ERR_IO;
    }

    if (oled_write_command(cfg, 0x22U) != OLED_OK){ /* Set page address */
        return OLED_ERR_IO;
    }
    if (oled_write_command(cfg, 0x00U) != OLED_OK){
        return OLED_ERR_IO;
    }
    if (oled_write_command(cfg, (uint8_t)(pages - 1U)) != OLED_OK){
        return OLED_ERR_IO;
    }

    return OLED_OK;
}

int32_t OLED_Init(const OLED_Config *cfg){
    if (oled_is_valid_config(cfg) == 0U){
        return OLED_ERR_INVALID_ARG;
    }

    uint8_t multiplex = (uint8_t)(cfg->height - 1U);
    uint8_t compins = (cfg->height == 64U) ? 0x12U : 0x02U;

    const uint8_t init_seq[] = {
        0xAEU,              /* Display OFF */
        0x20U, 0x00U,       /* Memory mode: horizontal addressing */
        0xB0U,              /* Page start address */
        0xC8U,              /* COM scan direction remapped */
        0x00U,              /* Low column start address */
        0x10U,              /* High column start address */
        0x40U,              /* Start line address */
        0x81U, 0x7FU,       /* Contrast */
        0xA1U,              /* Segment remap */
        0xA6U,              /* Normal display */
        0xA8U, multiplex,   /* Multiplex ratio */
        0xD3U, 0x00U,       /* Display offset */
        0xD5U, 0x80U,       /* Display clock divide */
        0xD9U, 0xF1U,       /* Pre-charge period */
        0xDAU, compins,     /* COM pins hardware config */
        0xDBU, 0x40U,       /* VCOMH deselect level */
        0x8DU, 0x14U,       /* Charge pump enable */
        0xAFU               /* Display ON */
    };

    for (size_t i = 0; i < sizeof(init_seq); i++){
        if (oled_write_command(cfg, init_seq[i]) != OLED_OK){
            return OLED_ERR_IO;
        }
    }

    return OLED_Clear(cfg);
}

int32_t OLED_Fill(const OLED_Config *cfg, uint8_t pattern){
    uint8_t payload[OLED_DATA_CHUNK_SIZE];
    size_t remaining;

    if (oled_is_valid_config(cfg) == 0U){
        return OLED_ERR_INVALID_ARG;
    }

    if (oled_set_full_window(cfg) != OLED_OK){
        return OLED_ERR_IO;
    }

    memset(payload, pattern, sizeof(payload));

    remaining = OLED_BufferSize(cfg);
    while (remaining > 0U){
        size_t chunk = (remaining > OLED_DATA_CHUNK_SIZE) ? OLED_DATA_CHUNK_SIZE : remaining;
        if (oled_write_data(cfg, payload, chunk) != OLED_OK){
            return OLED_ERR_IO;
        }
        remaining -= chunk;
    }

    return OLED_OK;
}

int32_t OLED_Clear(const OLED_Config *cfg){
    return OLED_Fill(cfg, 0x00U);
}

int32_t OLED_DrawBitmap(const OLED_Config *cfg, const uint8_t *bitmap, size_t length){
    if ((oled_is_valid_config(cfg) == 0U) || (bitmap == NULL)){
        return OLED_ERR_INVALID_ARG;
    }

    if (length != OLED_BufferSize(cfg)){
        return OLED_ERR_INVALID_ARG;
    }

    if (oled_set_full_window(cfg) != OLED_OK){
        return OLED_ERR_IO;
    }

    return oled_write_data(cfg, bitmap, length);
}

int32_t OLED_GfxFlushCallback(void *context,
                              const uint8_t *buffer,
                              size_t buffer_size,
                              uint16_t width,
                              uint16_t height)
{
    const OLED_Config *cfg = (const OLED_Config *)context;

    if (oled_is_valid_config(cfg) == 0U)
    {
        return OLED_ERR_INVALID_ARG;
    }

    if ((width != cfg->width) || (height != cfg->height))
    {
        return OLED_ERR_INVALID_ARG;
    }

    return OLED_DrawBitmap(cfg, buffer, buffer_size);
}
