#include <ESP32SPISlave.h>
#include <ArduinoJson.h>
#include <helper.h>

#define SPI_MODE SPI_MODE3

#define HSPI_MISO 9
#define HSPI_MOSI 10
#define HSPI_SCLK 8
#define HSPI_SS 7

ESP32SPISlave slave;

static constexpr uint32_t BUFFER_SIZE = 8; //spi transaction 8 bytes
static constexpr uint32_t QUEUE_SIZE = 2;
uint8_t tx_buf[BUFFER_SIZE] = {0,0,0,0,0,0,0,0}; // data to send back
uint8_t rx_buf[BUFFER_SIZE] = {0,0,0,0,0,0,0,0}; //data to receive

// Define magic pattern for start and stop
const uint8_t MAGIC_PATTERN[8] = {0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00};
int start_sending = 0;
int sent_count = 0;

void sendImage() {
    // Send start magic pattern
    memcpy(tx_buf, MAGIC_PATTERN, 8);
    slave.queue(tx_buf, NULL, BUFFER_SIZE);
    slave.wait();

    // Send image data
    for (uint32_t j = 0; j < sizeof(image); j += 4) {
        memset(tx_buf, 0, BUFFER_SIZE);
        tx_buf[0] = 0xA0;
        tx_buf[1] = 0xA0;
        memcpy(&tx_buf[2], &image[j], 4);
        tx_buf[6] = 0xA0;
        tx_buf[7] = 0xA0;

        slave.queue(tx_buf, NULL, BUFFER_SIZE);
        slave.wait();
    }

    // Send stop magic pattern
    memcpy(tx_buf, MAGIC_PATTERN, 8);
    slave.queue(tx_buf, NULL, BUFFER_SIZE);
    slave.wait();

    Serial.println("Transfer complete");
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    // pinMode(LED, OUTPUT);

    slave.setDataMode(SPI_MODE);   // MUSTx match Nios, this is the CPOL and CPHA
    slave.setQueueSize(QUEUE_SIZE); 
    slave.begin();
    Serial.println("SPI slave ready");
    Serial.println(sizeof(image));
}

void loop() {
    // ── Step 1: Pre-queue FIRST, before doing anything else ──
    memset(rx_buf, 0, BUFFER_SIZE);
    slave.queue(NULL, rx_buf, BUFFER_SIZE);  // queued and waiting BEFORE Nios sends
    slave.wait();                             // blocks until master clocks 8 bytes

    // ── Step 2: Now check what we received ──
    if (rx_buf[0] == 0x01 || rx_buf[3] == 0x01) {  // check both positions
        Serial.println("Trigger received, starting transfer...");
        sendImage();
    }
    // if not 0x01, loop back immediately and re-queue
}