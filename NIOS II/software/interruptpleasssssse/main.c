#include "system.h"
#include <stdio.h>
#include <stdint.h>
#include <system.h>
#include <io.h>
#include "altera_avalon_spi_regs.h"
#include <stddef.h>
#include "sys/alt_stdio.h"
#include "altera_avalon_pio_regs.h"
#include "alt_types.h"
#include <string.h>
#include "altera_up_avalon_accelerometer_spi.h"
#include "sys/alt_irq.h"

//base addresses
int* DRAM_WRADDR = (int*) 0x0200000;
#define SPI_BASE 0x04000000 //IORD_ALTERA_AVALON_SPI_STATUS expects an integer not a pointer
volatile int * PIXEL_EXPORT = (volatile int *)0x040000b0;
volatile int * ADDR_EXPORT  = (volatile int *)0x04000090;
volatile int * WREN_EXPORT  = (volatile int *)0x04000080;


//interrupt global variables
volatile int edge_capture;
volatile int start_spi = 0;
volatile int start_vga = 0;
//volatile int interrupt_flag = 0;

//function prototypes
void write_pixel(unsigned int addr, unsigned char color);
void display_6chars(char* msg, int offset, int len);
unsigned char get_seg7(char c);
uint32_t read_bytes();

//interrupt function prototype
static void init_key_pio();
static void handle_key_interrupts(void* context, alt_u32 id);

static void init_key_pio() {
	/* Recast the edge_capture pointer to match the
	alt_irq_register() function prototype. */
	void* edge_capture_ptr = (void*) &edge_capture;
	/* Enable all 2 button interrupts. */
	IOWR_ALTERA_AVALON_PIO_IRQ_MASK(PIO_7_BASE, 0x3);
	/* Reset the edge capture register. */
	IOWR_ALTERA_AVALON_PIO_EDGE_CAP(PIO_7_BASE, 0);
	/* Register the ISR. */
	alt_irq_register( PIO_7_IRQ,edge_capture_ptr,handle_key_interrupts);
}

static void handle_key_interrupts (void* context, alt_u32 id) {
	/* cast the context pointer to an integer pointer. */
	volatile int* edge_capture_ptr = (volatile int*) context;
	/*
	* Read the edge capture register on the button PIO.
	* Store value.
	*/
	int edge = IORD_ALTERA_AVALON_PIO_EDGE_CAP(PIO_7_BASE);

	// Set the flag
	*edge_capture_ptr = edge;
	if (edge & 0x1) start_spi = !start_spi;
	if (edge & 0x2) start_vga = !start_vga;
//	interrupt_flag = 1;


	/* Write to the edge capture register to reset it. */
	IOWR_ALTERA_AVALON_PIO_EDGE_CAP(PIO_7_BASE, 0);
	/* reset interrupt capability for the Button PIO. */
	IOWR_ALTERA_AVALON_PIO_IRQ_MASK(PIO_7_BASE, 0x3); // 10 bits
}

void write_pixel(unsigned int addr, unsigned char color){
	*(ADDR_EXPORT) = addr;
	*(PIXEL_EXPORT) = color & 0x0F;
	*(WREN_EXPORT) = 1;
	*(WREN_EXPORT) = 0;
}

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


uint32_t read_bytes(){
	//SPI_BASE + 0 -> RXDATA register
	//SPI_BASE + 1 -> TXDATA register
	//SPI_BASE + 2 -> STATUS register
	//SPI_BASE + 3 -> CONTROL register
	unsigned char tx = 0x00000000;
	IOWR_ALTERA_AVALON_SPI_SLAVE_SEL(SPI_BASE, 0x2);  // select (active low)

	while (!(IORD_ALTERA_AVALON_SPI_STATUS(SPI_BASE) & 0x40)); //Bit 6 (0x40) -> TRDY (Transmit Ready)
	// Write data
    IOWR_ALTERA_AVALON_SPI_TXDATA(SPI_BASE, tx);
    // Wait until data received
    while (!(IORD_ALTERA_AVALON_SPI_STATUS(SPI_BASE) & 0x80)); //Bit 7 (0x80) -> RRDY (Receive- Ready)
    // Return received data (now changed to receive 4 bytes)
    uint32_t rx = IORD_ALTERA_AVALON_SPI_RXDATA(SPI_BASE);
    IOWR_ALTERA_AVALON_SPI_SLAVE_SEL(SPI_BASE, 0x3);  // deselect
	alt_printf("RX: %x\n", rx);
    return rx;
}


int prev_sw1 = 0;
int scroll_timer = 0;
int accel_timer = 0;

int main()
{
	//initiate key interrupt
	init_key_pio();

	//initiate accelerometer
    alt_up_accelerometer_spi_dev *accel = alt_up_accelerometer_spi_open_dev("/dev/accelerometer_spi_0");
    if (!accel) {
    	alt_printf("Failed to open accelerometer\n");
        return 1;
    }
    else{
    	alt_printf("Accelerator initialized\n");
    }
    alt_32 x, y, z;

    uint8_t pattern_hits = 0;
    int offset_write = 0;
    int scroll_offset = 0;
    int offset_read = 0;
//    int start_spi = 0;
//    int start_vga = 0;
//	int prev_button0 = 1; //usually active low
//	int curr_button0;
//	int prev_button1 = 1;
//	int curr_button1;

    char* message = "ARIGA-67  ";
    int len = strlen(message); // length of message

    while(1)
    {
        int sw = IORD_ALTERA_AVALON_PIO_DATA(PIO_6_BASE);
        int sw1 = (sw >> 0) & 1;
        int sw2 = (sw >> 1) & 1;

//    	uint8_t key = IORD_ALTERA_AVALON_PIO_DATA(PIO_7_BASE);
//    	uint8_t key0 = (key >> 0) & 0x1;
//    	uint8_t key1 = (key >> 1) & 0x1;

//    	curr_button0 = key0;
//    	curr_button1 = key1;

        // detect OFF -> ON transition
        if (sw1 && !prev_sw1) {
        	scroll_offset = 0;  // reset to clean start
        }

//    	if(prev_button0 && !curr_button0){
//    		start_spi = !start_spi;
//    	}
//    	if(prev_button1 && !curr_button1){
//    		start_vga = !start_vga;
//    	}

    	if(start_spi){
			uint32_t rx = read_bytes();

			if((rx & 0xFFFFFFFF) == 0xFF00FF00){
				if (pattern_hits == 0){
					pattern_hits =1;
					offset_write = 0;
					continue; //skip this loop to avoid picking up last start byte
				}
				if(pattern_hits == 1){
					alt_printf("Image transfer complete!\n");
					start_spi = 0;
					pattern_hits = 0;
					continue;
				}
			}
			if(pattern_hits == 1){
				IOWR_32DIRECT(DRAM_WRADDR,offset_write,rx);
				offset_write += 4; //writing 32 bits
			}

		}
    	if(start_vga) {
			offset_read = 0;
			for (int y = 0; y < 240; y++) {
				for (int x = 0; x < 320; x++) {
					unsigned char color = IORD_8DIRECT(DRAM_WRADDR,offset_read);
					unsigned int addr = y*320 + x;
					write_pixel(addr,color);
					if((x-2)%8 == 1) offset_read++;

				}
				if((y-2)%20 == 1){
					offset_read = offset_read - 320/8;
				}
			}
			alt_printf("Image display complete!\n");
			start_vga = 0;
    	}

        if (!sw1) {
            // SW1 off - blank display
            IOWR_ALTERA_AVALON_PIO_DATA(PIO_8_BASE, 0xFFFFFF);
            IOWR_ALTERA_AVALON_PIO_DATA(PIO_9_BASE, 0xFFFFFF);
        } else if (sw1 && sw2) {
            // SW1 on + SW2 on - scroll message
        	// ONLY update the display if enough "loops" have passed
        	if (scroll_timer >= 5000) { // Adjust this number based on CPU speed
				display_6chars(message, scroll_offset, len);
				scroll_offset = (scroll_offset + 1) % len;
				scroll_timer = 0;
        	}
        	scroll_timer++;

        } else {
            // SW1 on only - static display (first 6 chars)
            display_6chars(message, scroll_offset, len);
        }
        prev_sw1 = sw1;
//        prev_button0 = key0;
//		prev_button1 = key1;

        if (accel_timer >= 100000){
            alt_up_accelerometer_spi_read_x_axis(accel, &x);
            alt_up_accelerometer_spi_read_y_axis(accel, &y);
            alt_up_accelerometer_spi_read_z_axis(accel, &z);
            printf("X=%ld, Y=%ld, Z=%ld\n", x, y, z);
            accel_timer = 0;
        }

//        //Service interrupt
//        if(interrupt_flag){
//        	interrupt_flag = 0;
//
//        }
        accel_timer++;
    }
    return 0;
}
