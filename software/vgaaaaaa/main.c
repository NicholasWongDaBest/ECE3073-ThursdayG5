#include <stddef.h>
#include "sys/alt_stdio.h"
volatile int * PIXEL_EXPORT = (volatile int *)0x040410a0;
volatile int * ADDR_EXPORT  = (volatile int *)0x04041080;
volatile int * WREN_EXPORT  = (volatile int *)0x04041070;

void write_pixel(unsigned int addr, unsigned char color){
	*(ADDR_EXPORT) = addr;
	*(PIXEL_EXPORT) = color & 0xF;
	*(WREN_EXPORT) = 1;
	*(WREN_EXPORT) = 0;
}

void draw_color_stripes(){
	for (int y = 0; y < 240; y++){
		for (int x = 0; x < 320; x++){
			unsigned int addr = y*320 + x;
			unsigned char color = x/20;
			write_pixel(addr,color);
		}
	}
}

void draw_vertical_stripes(){
	for (int y = 0; y < 240; y++){
		unsigned char color = y/15;
		for (int x = 0; x < 320; x++){
			unsigned int addr = y*320 + x;
			write_pixel(addr,color);
		}
	}
}

void VGATask(void* pdata){
	while(1){
//		draw_color_stripes();
//		OSTimeDlyHMSM(0,0,1,0);
//		draw_vertical_stripes();
//		OSTimeDlyHMSM(0,0,1,0);
		draw_vertical_stripes();
	}
}

int main(){
	alt_putstr("Hello from Nios II!\n");
    while(1){
        draw_color_stripes();
    }
	return 0;
}

