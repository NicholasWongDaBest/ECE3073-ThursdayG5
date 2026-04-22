/*
 * vga.c
 *
 *  Created on: 20 Apr 2026
 *      Author: yuen
 */

volatile int* OutPort_PIXEL;
volatile int* OutPort_ADDR;
volatile int* OutPort_WREN;

void write_pixel(unsigned int addr, unsigned char color) {
	*OutPort_ADDR = addr;
	*OutPort_PIXEL = color & 0xf;
	*OutPort_WREN = 1;
	*OutPort_WREN = 0;
}

void draw_color_stripes() {
	for (int y = 0; y < 240; y++) {
		for (int x = 0; x < 320; x++) {
			unsigned int addr = y * 320 + x;
			unsigned char color = x/20;
			write_pixel(addr, color);
		}
	}
}

void draw_vertical_stripes() {
	for (int y = 0; y < 240; y++) {
		unsigned char color = y/15;
		for (int x = 0; x < 320; x++) {
			unsigned int addr = y * 320 + x;
			write_pixel(addr, color);
		}
	}
}

void draw_full_white() {
	for (int y = 0; y < 240; y++) {
		for (int x = 0; x < 320; x++) {
			unsigned int addr = y * 320 + x;
			write_pixel(addr, 0xf);
		}
	}
}

void draw_full_black() {
	for (int y = 0; y < 240; y++) {
		for (int x = 0; x < 320; x++) {
			unsigned int addr = y * 320 + x;
			write_pixel(addr, 0x0);
		}
	}
}
