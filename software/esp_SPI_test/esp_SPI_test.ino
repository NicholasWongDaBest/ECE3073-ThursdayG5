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
    "0", "1", "A", "B", "C", "D", "E", "F", "G", "H",
    "I", "J", "2", "K", "L", "M", "N", "O", "P", "Q",
    "R", "S", "T", "3" , "U", "V", "W", "X", "Y", "Z",
    "4", "  5", "6", "7", "8", "9"
};
const int label_count = sizeof(my_labels) / sizeof(my_labels[0]);

// ---------------------------------------------------------------------------
// Core 0: inference loop
// Decodes into back_bitmap, then swaps front/back.
// No locking needed — Core 1 reads only from send_buf which Core 0 never touches.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Inference stats — tracks appearance count and best score per label
// ---------------------------------------------------------------------------
static int  label_count_tracker[66] = {0};   // appearance count per label
static int  label_best_score[66]    = {0};   // best score seen per label
static int label_score_sum[66]     = {0}; 

// Global top1 result — written by inference, read by sendImage
static char  top1_label[8]  = "?";
static uint8_t top1_avg     = 0;

void compute_top1() {
    int best_idx   = -1;
    int best_count = 0;
    int best_score = 0;

    for (int i = 0; i < label_count; i++) {
        if (label_count_tracker[i] == 0) continue;
        if (label_count_tracker[i] > best_count ||
           (label_count_tracker[i] == best_count && label_best_score[i] > best_score)) {
            best_idx   = i;
            best_count = label_count_tracker[i];
            best_score = label_best_score[i];
        }
    }

    if (best_idx >= 0) {
        // Compute average confidence for top1
        int avg = label_score_sum[best_idx] / label_count_tracker[best_idx];
        strncpy(top1_label, my_labels[best_idx], 7);
        top1_label[7] = '\0';
        top1_avg = (uint8_t)avg;
        Serial.printf("Top1: %s  count=%d  avg=%d\n",
            top1_label, best_count, avg);
    } else {
        strncpy(top1_label, "?", 7);
        top1_avg = 0;
    }
}

void inference_task(void* pdata) {
    Serial.println("[Core0] Inference task started");

    while (1) {
        int status = -1;
        while (status != 0) {
            status = AI.invoke(1, false, true);
            if (status != 0) { Serial.printf("[Core0] AI error %d\n", status); delay(100); }
        }

        for (int i = 0; i < (int)AI.boxes().size(); i++) {
            int idx = AI.boxes()[i].target;
            if (idx >= 0 && idx < label_count) {
                label_count_tracker[idx]++;
                label_score_sum[idx]  += AI.boxes()[i].score;
                if (AI.boxes()[i].score > label_best_score[idx])
                    label_best_score[idx] = AI.boxes()[i].score;
            }
        }

        // --- Capture sidebar data BEFORE resetting trackers ---
        InferEntry sidebar_entries[MAX_SIDEBAR_ENTRIES];
        int num_sidebar = 0;
        for (int i = 0; i < label_count && num_sidebar < MAX_SIDEBAR_ENTRIES; i++) {
            if (label_count_tracker[i] == 0) continue;
            strncpy(sidebar_entries[num_sidebar].label, my_labels[i], 7);
            sidebar_entries[num_sidebar].label[7] = '\0';
            sidebar_entries[num_sidebar].count    = label_count_tracker[i];
            sidebar_entries[num_sidebar].avg_conf =
                (uint8_t)(label_score_sum[i] / label_count_tracker[i]);
            num_sidebar++;
        }

        compute_top1();
        memset(label_count_tracker, 0, sizeof(label_count_tracker));
        memset(label_score_sum,     0, sizeof(label_score_sum));
        memset(label_best_score,    0, sizeof(label_best_score));

        String b64 = AI.last_image();
        if (b64.length() == 0) continue;
        if (!processBase64Jpeg(b64.c_str())) continue;

        // --- Render sidebar onto back_bitmap before swap ---
        renderSidebars(back_bitmap, sidebar_entries, num_sidebar);

        uint8_t* tmp = front_bitmap;
        front_bitmap = back_bitmap;
        back_bitmap  = tmp;
        final_bitmap = front_bitmap;
        frame_ready  = true;
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

    // Image data packets — unchanged
    uint32_t total = OUT_BUFFER_SIZE;
    for (uint32_t j = 0; j < total; j += PAYLOAD_BYTES) {
        memset(tx_buf, 0, BUFFER_SIZE);
        tx_buf[0] = 0xA0; tx_buf[1] = 0xA0;
        uint32_t chunk = ((j + PAYLOAD_BYTES) <= total) ? PAYLOAD_BYTES : (total - j);
        memcpy(&tx_buf[2], &send_buf[j], chunk);
        tx_buf[BUFFER_SIZE-2] = 0xA0;
        tx_buf[BUFFER_SIZE-1] = 0xA0;
        slave.queue(tx_buf, NULL, BUFFER_SIZE);
        slave.wait();
    }

    // --- Inference packet (NEW) ---
    memset(tx_buf, 0, BUFFER_SIZE);
    tx_buf[0] = 0xC0; tx_buf[1] = 0xC0;   // distinct marker, not A0 or FF
    strncpy((char*)&tx_buf[2], top1_label, 6);
    tx_buf[8]  = top1_avg;
    tx_buf[BUFFER_SIZE-2] = 0xC0;
    tx_buf[BUFFER_SIZE-1] = 0xC0;
    slave.queue(tx_buf, NULL, BUFFER_SIZE);
    slave.wait();

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
