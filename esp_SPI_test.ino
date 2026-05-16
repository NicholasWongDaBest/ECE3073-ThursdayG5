#include <ESP32SPISlave.h>
#include <ArduinoJson.h>
#include "helper.h"
#include "ImageTransform.h" // Ensures final_bitmap and processBase64Jpeg are available
#include <Seeed_Arduino_SSCMA.h>

#define SPI_MODE SPI_MODE3

#define HSPI_MISO 9
#define HSPI_MOSI 10
#define HSPI_SCLK 8
#define HSPI_SS 7

ESP32SPISlave slave;
SSCMA AI;

// Set this to match the Nios II PAYLOAD_BYTES exactly!
static constexpr uint32_t PAYLOAD_BYTES = 60; 
// Header(2) + Payload + Footer(2)
static constexpr uint32_t BUFFER_SIZE = PAYLOAD_BYTES + 4; 
static constexpr uint32_t QUEUE_SIZE = 2;

uint8_t tx_buf[BUFFER_SIZE]; 
uint8_t rx_buf[BUFFER_SIZE]; 

const uint8_t MAGIC_PATTERN[8] = {0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00};

// Your labels
const char* my_labels[] = {
"(", ")", ",", "-", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    ":", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", 
    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "_", 
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", 
    "o", "p", "r", "s", "t", "u", "v", "w", "y", "z"
};

const int label_count = sizeof(my_labels) / sizeof(my_labels[0]);

// --- SEND IMAGE FUNCTION ---
void sendImage() {
    Serial.println("Starting SPI Transfer to Nios...");
    
    // 1. Send start magic pattern
    memset(tx_buf, 0, BUFFER_SIZE);
    memcpy(tx_buf, MAGIC_PATTERN, 8);
    slave.queue(tx_buf, NULL, BUFFER_SIZE);
    slave.wait();

    // 2. Send image data in chunks
    // We are now using 'final_bitmap' from ImageTransform.h!
    for (uint32_t j = 0; j < sizeof(final_bitmap); j += PAYLOAD_BYTES) {
        memset(tx_buf, 0, BUFFER_SIZE);
        
        // Header
        tx_buf[0] = 0xA0;
        tx_buf[1] = 0xA0;
        
        // Safety check to avoid reading past the end of the array
        int chunk_size = PAYLOAD_BYTES;
        if (j + PAYLOAD_BYTES > sizeof(final_bitmap)) {
            chunk_size = sizeof(final_bitmap) - j;
        }
        
        // Payload (Using final_bitmap)
        memcpy(&tx_buf[2], &final_bitmap[j], chunk_size);
        
        // Footer
        tx_buf[BUFFER_SIZE - 2] = 0xA0;
        tx_buf[BUFFER_SIZE - 1] = 0xA0;

        slave.queue(tx_buf, NULL, BUFFER_SIZE);
        slave.wait();
    }

    // 3. Send stop magic pattern
    memset(tx_buf, 0, BUFFER_SIZE);
    memcpy(tx_buf, MAGIC_PATTERN, 8);
    slave.queue(tx_buf, NULL, BUFFER_SIZE);
    slave.wait();

    Serial.println("Transfer complete");
}

void setup() {
    AI.begin();
    Serial.begin(115200);
    delay(1000);

    slave.setDataMode(SPI_MODE);   
    slave.setQueueSize(QUEUE_SIZE); 
    slave.begin();
    Serial.println("SPI slave ready");
}

void loop() {
    // 1. Wait for Nios II to send a command over SPI
    memset(rx_buf, 0, BUFFER_SIZE);
    slave.queue(NULL, rx_buf, BUFFER_SIZE);  
    slave.wait(); // ESP32 PAUSES HERE until it receives SPI clocks                              

    // 2. Check if the command was the trigger (0x01)
    if (rx_buf[0] == 0x01 || rx_buf[3] == 0x01) { 
        Serial.println("Trigger (0x01) received! Capturing image...");
        Serial.println("Asking camera for inference + Base64 image...");
        
        // 3. Request inference AND the Base64 image (Saving the status code to debug)
        int ai_status = AI.invoke(1, false, true);
        
        // 0 indicates success in the Seeed library
        if (ai_status == 0) {
            Serial.println("\n--- Detection Result ---");

            // Print AI Detections (Boxes)
            for (int i = 0; i < AI.boxes().size(); i++) {
                int index = AI.boxes()[i].target;
                Serial.print("Detected: ");
                if (index < label_count) {
                    Serial.print(my_labels[index]);
                } else {
                    Serial.print("Unknown");
                }
                Serial.print(", Score: "); Serial.println(AI.boxes()[i].score);
            }

            // 4. Get the Image
            String base64_image = AI.last_image();
            Serial.println(base64_image);
            
            if (base64_image.length() > 0) {
                Serial.println("Image successfully received from camera! Decoding...");
                
                // 5. Decode the Base64 into the final_bitmap
                if (processBase64Jpeg(base64_image.c_str())) {
                    
                    // 6. Send it over SPI!
                    sendImage();
                    
                } else {
                    Serial.println("ERROR: Failed to process the JPEG data.");
                }
            } else {
                Serial.println("WARNING: AI ran successfully, but no image string was returned.");
            }
            
        } else {
            // If it fails, this will tell us exactly why
            Serial.print("ERROR: Camera failed or timed out! Status Code: ");
            Serial.println(ai_status);
            Serial.println("Try changing AI.invoke(1, false, true) back to AI.invoke() to see if it's an image streaming issue.");
        }
    }
}