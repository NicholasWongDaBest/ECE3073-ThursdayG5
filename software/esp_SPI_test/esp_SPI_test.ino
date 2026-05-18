// ESP32-S3 dual-core SPI slave + SSCMA camera
// Core 0: continuous AI inference + image decode → front/back buffer swap
// Core 1: waits for trigger → snapshots front_bitmap → sends over SPI

#include <ESP32SPISlave.h>
#include "ImageTransform.h"
#include <Seeed_Arduino_SSCMA.h>

#define PIN_MOSI  10
#define PIN_MISO  9
#define PIN_SCLK  8
#define PIN_CS    7
#define SPI_MODE  SPI_MODE3

static constexpr uint32_t PAYLOAD_BYTES = 60;
static constexpr uint32_t BUFFER_SIZE   = PAYLOAD_BYTES + 4;  // 64
static constexpr uint32_t QUEUE_SIZE    = 2;

const uint8_t MAGIC_PATTERN[8] = {0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00};

// DMA-safe buffers in internal DRAM
static DRAM_ATTR uint8_t tx_buf[BUFFER_SIZE] __attribute__((aligned(32)));
static DRAM_ATTR uint8_t rx_buf[BUFFER_SIZE] __attribute__((aligned(32)));

ESP32SPISlave slave;
SSCMA AI;

const char* my_labels[] = {
    "(", ")", ",", "-", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    ":", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "_",
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n",
    "o", "p", "r", "s", "t", "u", "v", "w", "y", "z"
};
const int label_count = sizeof(my_labels) / sizeof(my_labels[0]);

// ---------------------------------------------------------------------------
// Core 0: inference loop
// Decodes into back_bitmap, then swaps front/back.
// No locking needed — Core 1 reads only from send_buf which Core 0 never touches.
// ---------------------------------------------------------------------------
void inference_task(void* pdata) {
    Serial.println("[Core0] Inference task started");

    while (1) {
        // Run inference with retry
        int status = -1;
        while (status != 0) {
            status = AI.invoke(1, false, true);
            if (status != 0) {
                Serial.printf("[Core0] AI error %d, retrying...\n", status);
                delay(100);
            }
        }

        for (int i = 0; i < (int)AI.boxes().size(); i++) {
            int idx = AI.boxes()[i].target;
            Serial.printf("[Core0] Detected: %s  score=%d\n",
                (idx < label_count) ? my_labels[idx] : "?",
                AI.boxes()[i].score);
        }

        String b64 = AI.last_image();
        if (b64.length() == 0) continue;
        if (!processBase64Jpeg(b64.c_str())) continue;

        // Swap — back (just written) becomes new front
        uint8_t* tmp = front_bitmap;
        front_bitmap = back_bitmap;
        back_bitmap  = tmp;
        final_bitmap = front_bitmap;
        frame_ready  = true;   // signal Core 1 that a fresh frame is available
    }
}

// ---------------------------------------------------------------------------
// sendImage — sends from send_buf (private snapshot, Core 0 never touches it)
// ---------------------------------------------------------------------------
void sendImage() {
    // Start magic
    memset(tx_buf, 0, BUFFER_SIZE);
    memcpy(tx_buf, MAGIC_PATTERN, 8);
    slave.queue(tx_buf, NULL, BUFFER_SIZE);
    slave.wait();

    // Data packets
    uint32_t total = OUT_BUFFER_SIZE;
    for (uint32_t j = 0; j < total; j += PAYLOAD_BYTES) {
        memset(tx_buf, 0, BUFFER_SIZE);
        tx_buf[0] = 0xA0;
        tx_buf[1] = 0xA0;

        uint32_t chunk = ((j + PAYLOAD_BYTES) <= total) ? PAYLOAD_BYTES : (total - j);
        memcpy(&tx_buf[2], &send_buf[j], chunk);   // read from send_buf, not front_bitmap

        tx_buf[BUFFER_SIZE - 2] = 0xA0;
        tx_buf[BUFFER_SIZE - 1] = 0xA0;

        slave.queue(tx_buf, NULL, BUFFER_SIZE);
        slave.wait();
    }

    // Stop magic
    memset(tx_buf, 0, BUFFER_SIZE);
    memcpy(tx_buf, MAGIC_PATTERN, 8);
    slave.queue(tx_buf, NULL, BUFFER_SIZE);
    slave.wait();
}

// ---------------------------------------------------------------------------
// Core 1: SPI task
// On trigger: snapshot front_bitmap into send_buf, then send.
// Core 0 swaps freely the whole time — no locks needed.
// ---------------------------------------------------------------------------
void spi_task(void* pdata) {
    Serial.println("[Core1] SPI task started");

    while (1) {
        // Wait for trigger from Nios — exactly 4 bytes
        memset(rx_buf, 0, 4);
        slave.queue(NULL, rx_buf, 4);
        slave.wait();

        bool triggered = (rx_buf[0] == 0x01 || rx_buf[3] == 0x01);
        if (!triggered) continue;

        Serial.println("[Core1] Trigger received");

        // Wait for Core 0 to have at least one fresh frame ready
        while (!frame_ready) { delay(10); }
        frame_ready = false;

        // Snapshot front_bitmap into send_buf (~1ms for 76800 bytes from PSRAM)
        // After this point Core 0 can swap front/back as many times as it likes
        // without affecting our transmission — we read only from send_buf
        memcpy(send_buf, front_bitmap, OUT_BUFFER_SIZE);

        Serial.println("[Core1] Sending frame...");
        sendImage();
        Serial.println("[Core1] Frame sent");
    }
}

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.printf("PSRAM size: %u bytes\n", ESP.getPsramSize());
    if (ESP.getPsramSize() == 0) {
        Serial.println("FATAL: PSRAM not detected — set Tools->PSRAM->OPI PSRAM");
        while (1);
    }

    if (!initFinalBitmap()) {
        Serial.println("FATAL: buffer init failed");
        while (1);
    }

    Serial.printf("front_bitmap @ %p\n", front_bitmap);
    Serial.printf("back_bitmap  @ %p\n", back_bitmap);
    Serial.printf("send_buf     @ %p\n", send_buf);
    Serial.printf("Free internal RAM: %u bytes\n",
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    slave.setDataMode(SPI_MODE);
    slave.setQueueSize(QUEUE_SIZE);
    slave.begin();

    AI.begin();

    xTaskCreatePinnedToCore(inference_task, "inference", 8192, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(spi_task,       "spi",       4096, NULL, 1, NULL, 1);

    Serial.println("[ESP32] Both tasks launched — ready");
}

void loop() {
    delay(1000);
}
