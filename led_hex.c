#include "system.h"
#include "altera_avalon_pio_regs.h"
#include "alt_types.h"
#include "unistd.h"

#define SPEAKER_BASE 0x4041020

// 7-segment encoding (active low) for A-Z and space
unsigned char seg7_alpha[] = {
    0x88, // A
    0x83, // B
    0xC6, // C
    0xA1, // D
    0x86, // E
    0x8E, // F
    0xC2, // G (Use 0xC2 if don't want it to look like a 6 as 0X82 makes it look like a 6)
    0x89, // H
    0xF9, // I
    0xE1, // J
    0x89, // K
    0xC7, // L
    0xC8, // M
    0xAB, // N
    0xC0, // O
    0x8C, // P
    0x98, // Q
    0xAF, // R
    0x92, // S
    0x87, // T
    0xC1, // U
    0xC1, // V
    0xC1, // W
    0x89, // X
    0x91, // Y
    0xA4, // Z
    0xFF  // space
};

unsigned char seg7_numbers[] = {
    0xC0, // 0
    0xF9, // 1
    0xA4, // 2
    0xB0, // 3
    0x99, // 4
    0x92, // 5
    0x82, // 6
    0xF8, // 7
    0x80, // 8
    0x90  // 9
};

unsigned char get_seg7(char c) {
    // letters
    if (c >= 'A' && c <= 'Z') return seg7_alpha[c - 'A'];
    if (c >= 'a' && c <= 'z') return seg7_alpha[c - 'a'];
    // numbers
    if (c >= '0' && c <= '9') return seg7_numbers[c - '0'];
    // special characters
    if (c == ' ') return 0xFF; // blank
    if (c == '-') return 0xBF; // dash
    if (c == '.') return 0x7F; // dot
    if (c == '_') return 0xE7; // underscore
    if (c == '?') return 0x86; // question mark (approximate)
    return 0xFF; // blank for anything else
}

void display_6chars(char* msg, int offset, int len) {
    char chars[6];
    for (int i = 0; i < 6; i++) {
        chars[i] = msg[(offset + i) % len];
    }

    unsigned int hex012 = (get_seg7(chars[3]) << 16) |
                          (get_seg7(chars[4]) << 8)  |
                           get_seg7(chars[5]);

    unsigned int hex345 = (get_seg7(chars[0]) << 16) |
                          (get_seg7(chars[1]) << 8)  |
                           get_seg7(chars[2]);

    IOWR_ALTERA_AVALON_PIO_DATA(PIO_8_BASE, hex012); // HEX012
    IOWR_ALTERA_AVALON_PIO_DATA(PIO_9_BASE, hex345); // HEX345
}

void delay_ms(int ms) {
    volatile int i;
    for (i = 0; i < ms * 5000; i++);
}

void play_tone(int freq_hz, int duration_ms) {
    int half_period_us = 1000000 / (freq_hz * 2);
    int cycles = (duration_ms * freq_hz) / 1000;
    int i;
    for (i = 0; i < cycles; i++) {
        IOWR_ALTERA_AVALON_PIO_DATA(SPEAKER_BASE, 1); // high
        usleep(half_period_us);
        IOWR_ALTERA_AVALON_PIO_DATA(SPEAKER_BASE, 0); // low
        usleep(half_period_us);
    }
}

int prev_sw1 = 0;
int offset = 0;

int main() {
    char* message = "ARIGA-67  ";
    int len = strlen(message); // length of message

    // Can use this if want to show leds just like that
    // IOWR_ALTERA_AVALON_PIO_DATA(PIO_5_BASE, 0x3FF); // force all LEDs on

    while (1) {
        int sw = IORD_ALTERA_AVALON_PIO_DATA(PIO_6_BASE);
        int sw1 = (sw >> 0) & 1;
        int sw2 = (sw >> 1) & 1;
        int sw3 = (sw >> 2) & 1;
        int sw4 = (sw >> 3) & 1;

        // detect OFF -> ON transition
        if (sw1 && !prev_sw1) {
            offset = 0;  // reset to clean start
        }

        if (!sw1) {
            // SW1 off - blank display
            IOWR_ALTERA_AVALON_PIO_DATA(PIO_8_BASE, 0xFFFFFF);
            IOWR_ALTERA_AVALON_PIO_DATA(PIO_9_BASE, 0xFFFFFF);
        } else if (sw1 && sw2) {
            // SW1 on + SW2 on - scroll message
            display_6chars(message, offset, len);
            offset = (offset + 1) % len;
            delay_ms(300);
        } else {
            // SW1 on only - static display (first 6 chars)
            display_6chars(message, offset, len);
        }

        // LEDs run alongside - just mirror SW1
		if (sw1) {
			IOWR_ALTERA_AVALON_PIO_DATA(PIO_5_BASE, 0x3FF); // all on
		} else {
			IOWR_ALTERA_AVALON_PIO_DATA(PIO_5_BASE, 0x000); // all off
		}

		// SW4 controls speaker
			if (sw4) {
				play_tone(440, 500); // 440hz = A note, 500ms
			}

		prev_sw1 = sw1;
    }
    return 0;
}

