#include "system.h"
#include <stdio.h>
#include <stdint.h>
#include <system.h>
#include <io.h>
#include "altera_avalon_spi_regs.h"
#include <stddef.h>
#include "sys/alt_stdio.h"

int* DRAM_WRADDR = (int*) 0x0000000;
#define SPI_BASE 0x04020000 //IORD_ALTERA_AVALON_SPI_STATUS expects an integer not a pointer


int main()
{
	//SPI_BASE + 0 -> RXDATA register
	//SPI_BASE + 1 -> TXDATA register
	//SPI_BASE + 2 -> STATUS register
	//SPI_BASE + 3 -> CONTROL register

    unsigned char tx = 0x00;
    uint32_t byte_history = 0;
    uint8_t pattern_hits = 0;
    int offset = 0;

    while(1)
    {
    	IOWR_ALTERA_AVALON_SPI_SLAVE_SEL(SPI_BASE, 0x2);  // select (active low)

    	while (!(IORD_ALTERA_AVALON_SPI_STATUS(SPI_BASE) & 0x40)); //Bit 6 (0x40) -> TRDY (Transmit Ready)
    	// Write data
        IOWR_ALTERA_AVALON_SPI_TXDATA(SPI_BASE, tx);
        // Wait until data received
        while (!(IORD_ALTERA_AVALON_SPI_STATUS(SPI_BASE) & 0x80)); //Bit 7 (0x80) -> RRDY (Receive- Ready)
        // Return received data
        unsigned char rx = IORD_ALTERA_AVALON_SPI_RXDATA(SPI_BASE);
        alt_printf("RX: %x\n", rx);
        IOWR_ALTERA_AVALON_SPI_SLAVE_SEL(SPI_BASE, 0x3);  // deselect

        byte_history = (byte_history << 8)|rx; //save into a sequence of bytes
        if((byte_history & 0xFFFFFFFF) == 0xFF00FF00){
        	if (pattern_hits == 0){
        		pattern_hits =1;
        		offset = 0;
        		continue; //skip this loop to avoid picking up last start byte
        	}
        	if(pattern_hits == 1){
        		alt_printf("Image transfer complete!\n");
        		pattern_hits = 0;
        		byte_history = 0;
        		continue;
        	}

        }

        if(pattern_hits == 1){
        	IOWR_32DIRECT(DRAM_WRADDR,offset,rx);
        	offset += 4;
        }

    }
}
