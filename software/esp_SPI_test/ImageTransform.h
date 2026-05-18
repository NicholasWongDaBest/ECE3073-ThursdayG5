#ifndef IMAGETRANSFORM_H
#define IMAGETRANSFORM_H

#include <Arduino.h>
#include <string.h>
#include "mbedtls/base64.h"
#include <JPEGDEC.h>

#define OUT_WIDTH       320
#define OUT_HEIGHT      240
#define OUT_BUFFER_SIZE (OUT_WIDTH * OUT_HEIGHT)   // 76800 bytes
#define DECODED_IMAGE_MAX_SIZE (15 * 1024)

// ---------------------------------------------------------------------------
// Three buffers, all in PSRAM:
//   front_bitmap — Core 0 just finished writing here (after swap)
//   back_bitmap  — Core 0 is currently writing here
//   send_buf     — Core 1 sends from here; Core 0 never touches it
// ---------------------------------------------------------------------------
uint8_t*       front_bitmap = nullptr;
uint8_t*       back_bitmap  = nullptr;
uint8_t*       send_buf     = nullptr;
uint8_t*       final_bitmap = nullptr;   // alias for front_bitmap
unsigned char* jpegImage    = nullptr;

volatile bool frame_ready = false;   // Core 0 sets after swap, Core 1 clears

JPEGDEC jpeg;

static uint8_t* raw_decode_buf = nullptr;
static int      raw_w          = 0;
static int      raw_h          = 0;

// ---------------------------------------------------------------------------
// initFinalBitmap — call once in setup() before creating any tasks
// ---------------------------------------------------------------------------
bool initFinalBitmap() {
    front_bitmap = (uint8_t*)       heap_caps_malloc(OUT_BUFFER_SIZE,            MALLOC_CAP_SPIRAM);
    back_bitmap  = (uint8_t*)       heap_caps_malloc(OUT_BUFFER_SIZE,            MALLOC_CAP_SPIRAM);
    send_buf     = (uint8_t*)       heap_caps_malloc(OUT_BUFFER_SIZE,            MALLOC_CAP_SPIRAM);
    jpegImage    = (unsigned char*) heap_caps_malloc(DECODED_IMAGE_MAX_SIZE + 1, MALLOC_CAP_SPIRAM);

    if (!front_bitmap || !back_bitmap || !send_buf || !jpegImage) {
        Serial.println("ERROR: PSRAM alloc failed");
        return false;
    }

    memset(front_bitmap, 0, OUT_BUFFER_SIZE);
    memset(back_bitmap,  0, OUT_BUFFER_SIZE);
    memset(send_buf,     0, OUT_BUFFER_SIZE);
    final_bitmap = front_bitmap;
    return true;
}

// ---------------------------------------------------------------------------
// JPEGDraw callback — RGB565 → RGB332 into raw_decode_buf (internal RAM)
// ---------------------------------------------------------------------------
int JPEGDraw(JPEGDRAW* pDraw) {
    if (!raw_decode_buf) return 0;

    for (int y = 0; y < pDraw->iHeight; y++) {
        for (int x = 0; x < pDraw->iWidth; x++) {
            uint16_t rgb = pDraw->pPixels[y * pDraw->iWidth + x];

            uint8_t r = (rgb >> 13) & 0x07;
            uint8_t g = (rgb >> 8)  & 0x07;
            uint8_t b = (rgb >> 3)  & 0x03;

            uint8_t rgb332 = (r << 5) | (g << 2) | b;

            int dest_x = pDraw->x + x;
            int dest_y = pDraw->y + y;

            if (dest_x < raw_w && dest_y < raw_h)
                raw_decode_buf[dest_y * raw_w + dest_x] = rgb332;
        }
    }
    return 1;
}

// ---------------------------------------------------------------------------
// processBase64Jpeg — decodes into back_bitmap
// Caller swaps buffers after this returns true
// ---------------------------------------------------------------------------
bool processBase64Jpeg(const char* base64_str) {
    if (!base64_str)  return false;
    if (!back_bitmap) { Serial.println("ERROR: back_bitmap null"); return false; }
    if (!jpegImage)   { Serial.println("ERROR: jpegImage null");   return false; }

    // 1. Base64 decode
    size_t str_len    = strlen(base64_str);
    size_t output_len = 0;

    int ret = mbedtls_base64_decode(NULL, 0, &output_len,
                                    (const unsigned char*)base64_str, str_len);
    if (ret == MBEDTLS_ERR_BASE64_INVALID_CHARACTER) {
        Serial.println("ERROR: invalid base64 character");
        return false;
    }
    if (output_len == 0 || output_len > DECODED_IMAGE_MAX_SIZE) {
        Serial.printf("ERROR: decoded JPEG size %u out of range\n", output_len);
        return false;
    }

    ret = mbedtls_base64_decode(jpegImage, DECODED_IMAGE_MAX_SIZE, &output_len,
                                 (const unsigned char*)base64_str, str_len);
    if (ret != 0) {
        Serial.println("ERROR: base64 decode failed");
        return false;
    }

    // 2. Open JPEG
    if (!jpeg.openRAM(jpegImage, output_len, JPEGDraw)) {
        Serial.println("ERROR: JPEGDEC openRAM failed");
        return false;
    }

    raw_w = jpeg.getWidth();
    raw_h = jpeg.getHeight();

    // 3. Temporary decode buffer in fast internal RAM
    raw_decode_buf = (uint8_t*) heap_caps_malloc(raw_w * raw_h, MALLOC_CAP_INTERNAL);
    if (!raw_decode_buf) {
        Serial.printf("ERROR: cannot alloc %d bytes internal RAM\n", raw_w * raw_h);
        jpeg.close();
        return false;
    }

    // 4. Decode JPEG → RGB332 into raw_decode_buf, then scale into back_bitmap
    bool success = false;
    if (jpeg.decode(0, 0, 0)) {
        if (raw_w == OUT_WIDTH && raw_h == OUT_HEIGHT) {
            memcpy(back_bitmap, raw_decode_buf, OUT_BUFFER_SIZE);
            success = true;
        } else {
            float x_ratio = (float)raw_w / OUT_WIDTH;
            float y_ratio = (float)raw_h / OUT_HEIGHT;
            for (int y = 0; y < OUT_HEIGHT; y++) {
                for (int x = 0; x < OUT_WIDTH; x++) {
                    int px = (int)(x * x_ratio);
                    int py = (int)(y * y_ratio);
                    back_bitmap[y * OUT_WIDTH + x] =
                        raw_decode_buf[py * raw_w + px];
                }
            }
            success = true;
        }
    } else {
        Serial.println("ERROR: JPEGDEC decode failed");
    }

    // 5. Cleanup
    heap_caps_free(raw_decode_buf);
    raw_decode_buf = nullptr;
    jpeg.close();

    return success;
}

#endif // IMAGETRANSFORM_H