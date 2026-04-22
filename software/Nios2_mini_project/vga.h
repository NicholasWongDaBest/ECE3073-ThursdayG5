/*
 * vga.h
 *
 *  Created on: 20 Apr 2026
 *      Author: yuen
 */

#ifndef VGA_H_
#define VGA_H_

void write_pixel(unsigned int addr, unsigned char color);
void draw_color_stripes();
void draw_vertical_stripes();
void draw_full_white();
void draw_full_black();

#endif /* VGA_H_ */
