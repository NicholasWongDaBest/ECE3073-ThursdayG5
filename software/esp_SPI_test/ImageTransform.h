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
        // Clear entire buffer to black first
        memset(back_bitmap, 0, OUT_BUFFER_SIZE);

        // 240x240 image centered on 320x240 display
        // Left padding: (320 - 240) / 2 = 40 pixels
        int pad_left = (OUT_WIDTH - raw_w) / 2;

        if (raw_w <= OUT_WIDTH && raw_h <= OUT_HEIGHT) {
            // Direct copy — no scaling, just offset into center
            for (int y = 0; y < raw_h; y++) {
                for (int x = 0; x < raw_w; x++) {
                    back_bitmap[y * OUT_WIDTH + pad_left + x] =
                        raw_decode_buf[y * raw_w + x];
                }
            }
            success = true;
        } else {
            // Fallback: scale if camera returns larger than expected
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

#define MAX_SIDEBAR_ENTRIES 20

typedef struct {
    char    label[8];
    int     count;
    uint8_t avg_conf;
} InferEntry;

// Lookup string — index into FONT_DATA by finding char position here
static const char FONT_CHARS[] = " -:0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

// 4×6 font: each glyph is 6 bytes (one per row), bits 3..0 = pixels left→right
static const uint8_t FONT_DATA[39][6] = {
    {0x0,0x0,0x0,0x0,0x0,0x0}, // ' '
    {0x0,0x0,0xF,0x0,0x0,0x0}, // '-'
    {0x0,0x6,0x6,0x0,0x6,0x6}, // ':'
    {0x6,0x9,0x9,0x9,0x9,0x6}, // '0'
    {0x2,0x6,0x2,0x2,0x2,0x7}, // '1'
    {0x6,0x9,0x1,0x2,0x4,0xF}, // '2'
    {0xE,0x1,0x6,0x1,0x1,0xE}, // '3'
    {0x9,0x9,0xF,0x1,0x1,0x1}, // '4'
    {0xF,0x8,0xE,0x1,0x1,0xE}, // '5'
    {0x6,0x8,0xE,0x9,0x9,0x6}, // '6'
    {0xF,0x1,0x2,0x4,0x4,0x4}, // '7'
    {0x6,0x9,0x6,0x9,0x9,0x6}, // '8'
    {0x6,0x9,0x7,0x1,0x9,0x6}, // '9'
    {0x6,0x9,0x9,0xF,0x9,0x9}, // 'A'
    {0xE,0x9,0xE,0x9,0x9,0xE}, // 'B'
    {0x6,0x9,0x8,0x8,0x9,0x6}, // 'C'
    {0xE,0x9,0x9,0x9,0x9,0xE}, // 'D'
    {0xF,0x8,0xE,0x8,0x8,0xF}, // 'E'
    {0xF,0x8,0xE,0x8,0x8,0x8}, // 'F'
    {0x6,0x9,0x8,0xB,0x9,0x6}, // 'G'
    {0x9,0x9,0xF,0x9,0x9,0x9}, // 'H'
    {0xE,0x4,0x4,0x4,0x4,0xE}, // 'I'
    {0x3,0x1,0x1,0x1,0x9,0x6}, // 'J'
    {0x9,0xA,0xC,0xC,0xA,0x9}, // 'K'
    {0x8,0x8,0x8,0x8,0x8,0xF}, // 'L'
    {0x9,0xF,0x9,0x9,0x9,0x9}, // 'M'
    {0x9,0xD,0xB,0x9,0x9,0x9}, // 'N'
    {0x6,0x9,0x9,0x9,0x9,0x6}, // 'O'
    {0xE,0x9,0x9,0xE,0x8,0x8}, // 'P'
    {0x6,0x9,0x9,0x9,0xD,0x6}, // 'Q'
    {0xE,0x9,0x9,0xE,0xA,0x9}, // 'R'
    {0x7,0x8,0x6,0x1,0x1,0xE}, // 'S'
    {0xF,0x4,0x4,0x4,0x4,0x4}, // 'T'
    {0x9,0x9,0x9,0x9,0x9,0x6}, // 'U'
    {0x9,0x9,0x9,0x9,0x6,0x6}, // 'V'
    {0x9,0x9,0xF,0xF,0x9,0x9}, // 'W'
    {0x9,0x9,0x6,0x6,0x9,0x9}, // 'X'
    {0x9,0x9,0x6,0x4,0x4,0x4}, // 'Y'
    {0xF,0x1,0x2,0x4,0x8,0xF}, // 'Z'
};

static const uint8_t* sb_get_glyph(char c) {
    // uppercase only
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    const char* p = strchr(FONT_CHARS, c);
    return FONT_DATA[p ? (p - FONT_CHARS) : 0];
}

static void sb_draw_char(uint8_t* bmp, int x, int y, char c, uint8_t color) {
    const uint8_t* g = sb_get_glyph(c);
    for (int row = 0; row < 6; row++) {
        for (int col = 0; col < 4; col++) {
            if (!(g[row] & (0x8 >> col))) continue;
            int px = x + col, py = y + row;
            if (px >= 0 && px < OUT_WIDTH && py >= 0 && py < OUT_HEIGHT)
                bmp[py * OUT_WIDTH + px] = color;
        }
    }
}

static void sb_draw_str(uint8_t* bmp, int x, int y, const char* s, uint8_t color) {
    while (*s) { sb_draw_char(bmp, x, y, *s++, color); x += 5; }
}

// Draw a horizontal separator line across one sidebar
static void sb_draw_line(uint8_t* bmp, int x_start, int width, int y, uint8_t color) {
    for (int x = x_start; x < x_start + width && x < OUT_WIDTH; x++)
        if (y >= 0 && y < OUT_HEIGHT)
            bmp[y * OUT_WIDTH + x] = color;
}

// Sort entries descending by count (insertion sort, small n)
static void sb_sort(InferEntry* e, int n) {
    for (int i = 1; i < n; i++) {
        InferEntry key = e[i];
        int j = i - 1;
        while (j >= 0 && e[j].count < key.count) { e[j+1] = e[j]; j--; }
        e[j+1] = key;
    }
}

void renderSidebars(uint8_t* bmp, InferEntry* entries, int num) {
    if (num <= 0) return;
    sb_sort(entries, num);

    // Colors in RGB332: white=0xFF, grey=0x92, dark=0x49
    const uint8_t COL_HDR  = 0xFF;  // white header text
    const uint8_t COL_TEXT = 0xB6;  // light grey data text
    const uint8_t COL_LINE = 0x49;  // dark grey separator

    // --- Headers ---
    // Left:  "CLS" + ":N"  →  "CLS:N"
    // Right: "CONF"
    sb_draw_str(bmp,   1, 1, "CLS:N", COL_HDR);
    sb_draw_str(bmp, 281, 1, "CONF",  COL_HDR);

    // Separator line under header
    sb_draw_line(bmp,   0, 39, 8,  COL_LINE);
    sb_draw_line(bmp, 280, 39, 8,  COL_LINE);

    int y = 10;  // first entry row
    char buf[16];

    for (int i = 0; i < num && i < MAX_SIDEBAR_ENTRIES; i++) {
        if (y + 6 > OUT_HEIGHT) break;

        // Trim leading spaces from label (fixes "  5" style labels)
        char lbl[8];
        strncpy(lbl, entries[i].label, 7);
        lbl[7] = '\0';
        char* p = lbl;
        while (*p == ' ') p++;

        // Left sidebar: "A:03"  (label + colon + 2-digit count)
        snprintf(buf, sizeof(buf), "%s:%02d", p, entries[i].count);
        sb_draw_str(bmp, 1, y, buf, COL_TEXT);

        // Right sidebar: "087"  (3-digit avg confidence, left-aligned)
        snprintf(buf, sizeof(buf), "%03d", entries[i].avg_conf);
        sb_draw_str(bmp, 281, y, buf, COL_TEXT);

        y += 8;  // 6px glyph + 2px gap

        // Thin separator between entries (subtle)
        if (i < num - 1)
            sb_draw_line(bmp, 0, 39, y - 1, COL_LINE);
    }
}

#endif // IMAGETRANSFORM_H