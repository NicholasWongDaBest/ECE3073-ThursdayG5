/*
 * main.c
 *
 *  Created on: 18 Apr 2026
 *      Author: yuen
 */

#include "hex.h"
#include "vga.h"
#include "utility.h"

#include "limits.h"
#include "stdio.h"
#include "sys/alt_stdio.h"
#include "altera_up_avalon_accelerometer_spi.h"

volatile int* InPort_KEY = (int*) 0x04041040;
volatile int* InPort_SW = (int*) 0x04041050;
volatile int* OutPort_LEDR = (int*) 0x04041060;
volatile int* OutPort_HEX012 = (int*) 0x04041030;
volatile int* OutPort_HEX345 = (int*) 0x04041020;
volatile int* OutPort_PIXEL = (int*) 0x040410a0;
volatile int* OutPort_ADDR = (int*) 0x04041080;
volatile int* OutPort_WREN = (int*) 0x04041070;

alt_up_accelerometer_spi_dev *acc_dev;
int x_axis, y_axis, z_axis;
int prev_x_axis, prev_y_axis, prev_z_axis;

int main(void) {
	unsigned int offset = 0;
	int prev_sw = *InPort_SW;

	*OutPort_HEX012 = 0xffffff;
	*OutPort_HEX345 = 0xffffff;
	*OutPort_LEDR = 0x000;

	acc_dev = alt_up_accelerometer_spi_open_dev("/dev/accelerometer_spi_0");

	if (acc_dev == NULL) {
		*OutPort_LEDR = 0x3ff;
		while (1);
	}

	while (1) {
		if (*InPort_SW == 0x001) {
			display_chars("Bonjour", offset, 7);
		} else if (*InPort_SW == 0x003) {
			display_chars("Bonjour", offset, 7);
			offset = (offset + 1) % 7;
			usleep(300000);
		} else if (*InPort_SW == 0x000) {
			*OutPort_HEX012 = 0xffffff;
			*OutPort_HEX345 = 0xffffff;
		} else if (*InPort_SW == 0x200) {
			draw_color_stripes();
			while (*InPort_SW == 0x200);
		} else if (*InPort_SW == 0x100) {
			draw_vertical_stripes();
			while (*InPort_SW == 0x100);
		} else if (*InPort_SW == 0x004) {
			alt_up_accelerometer_spi_read_x_axis(acc_dev, &x_axis);
			alt_up_accelerometer_spi_read_y_axis(acc_dev, &y_axis);
			alt_up_accelerometer_spi_read_z_axis(acc_dev, &z_axis);

			if (x_axis >= -270 & x_axis < -216) {
				*OutPort_LEDR = 0x200;
			} else if (x_axis >= -216 & x_axis < -162) {
				*OutPort_LEDR = 0x100;
			} else if (x_axis >= -162 & x_axis < -108) {
				*OutPort_LEDR = 0x080;
			} else if (x_axis >= -108 & x_axis < -54) {
				*OutPort_LEDR = 0x040;
			} else if (x_axis >= -54 & x_axis < 0) {
				*OutPort_LEDR = 0x020;
			} else if (x_axis >= 0 & x_axis < 54) {
				*OutPort_LEDR = 0x010;
			} else if (x_axis >= 54 & x_axis < 108) {
				*OutPort_LEDR = 0x008;
			} else if (x_axis >= 108 & x_axis < 162) {
				*OutPort_LEDR = 0x004;
			} else if (x_axis >= 162 & x_axis < 216) {
				*OutPort_LEDR = 0x002;
			} else if (x_axis >= 216 & x_axis <= 270) {
				*OutPort_LEDR = 0x001;
			}
		}
	}
}
