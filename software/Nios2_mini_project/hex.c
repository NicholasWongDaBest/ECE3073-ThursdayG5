/*
 * hex.c
 *
 *  Created on: 20 Apr 2026
 *      Author: yuen
 */

volatile int* OutPort_HEX012;
volatile int* OutPort_HEX345;

unsigned char seg7_alpha[] = {
    0x88, // A
    0x83, // B
    0xc6, // C
    0xa1, // D
    0x86, // E
    0x8e, // F
    0xc2, // G (Use 0xC2 if don't want it to look like a 6 as 0X82 makes it look like a 6)
    0x89, // H
    0xf9, // I
    0xe1, // J
    0x89, // K
    0xc7, // L
    0xc8, // M
    0xab, // N
    0xc0, // O
    0x8c, // P
    0x98, // Q
    0xaf, // R
    0x92, // S
    0x87, // T
    0xc1, // U
    0xc1, // V
    0xc1, // W
    0x89, // X
    0x91, // Y
    0xa4, // Z
    0xff  // space
};

unsigned char seg7_numbers[] = {
    0xC0, // 0
    0xf9, // 1
    0xa4, // 2
    0xb0, // 3
    0x99, // 4
    0x92, // 5
    0x82, // 6
    0xf8, // 7
    0x80, // 8
    0x90  // 9
};

unsigned char get_char(char c) {
	if (c >= 'A' && c <= 'Z') {
		return seg7_alpha[c - 'A'];
	}
	if (c >= 'a' && c <= 'z') {
		return seg7_alpha[c - 'a'];
	}
	if (c >= '0' && c <= '9') {
		return seg7_numbers[c - '0'];
	}
	return 0xff;
}

void display_chars(char* message, int offset, int len) {
	char chars[6];
	for (int i = 0; i < 6; i++) {
		chars[i] = message[(offset + i) % len];
	}

	*OutPort_HEX012 = (get_char(chars[3]) << 16) | (get_char(chars[4]) << 8) | (get_char(chars[5]));
	*OutPort_HEX345 = (get_char(chars[0]) << 16) | (get_char(chars[1]) << 8) | (get_char(chars[2]));
}
