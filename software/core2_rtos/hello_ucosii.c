/*************************************************************************
* Copyright (c) 2004 Altera Corporation, San Jose, California, USA.      *
* All rights reserved. All use of this software and documentation is     *
* subject to the License Agreement located at the end of this file below.*
**************************************************************************
* Description:                                                           *
* The following is a simple hello world program running MicroC/OS-II.The * 
* purpose of the design is to be a very simple application that just     *
* demonstrates MicroC/OS-II running on NIOS II.The design doesn't account*
* for issues such as checking system call return codes. etc.             *
*                                                                        *
* Requirements:                                                          *
*   -Supported Example Hardware Platforms                                *
*     Standard                                                           *
*     Full Featured                                                      *
*     Low Cost                                                           *
*   -Supported Development Boards                                        *
*     Nios II Development Board, Stratix II Edition                      *
*     Nios Development Board, Stratix Professional Edition               *
*     Nios Development Board, Stratix Edition                            *
*     Nios Development Board, Cyclone Edition                            *
*   -System Library Settings                                             *
*     RTOS Type - MicroC/OS-II                                           *
*     Periodic System Timer                                              *
*   -Know Issues                                                         *
*     If this design is run on the ISS, terminal output will take several*
*     minutes per iteration.                                             *
**************************************************************************/

//core 2
#include <image.h>
#include "system.h"
#include <stdio.h>
#include <stdint.h>
#include <system.h>
#include <io.h>
#include <stddef.h>
#include "sys/alt_stdio.h"
#include "altera_avalon_pio_regs.h"
#include "alt_types.h"
#include <string.h>
#include "sys/alt_irq.h"
#include "unistd.h"
#include "includes.h"
// ... [Include your standard headers here] ...

// --- ADD RTOS GLOBALS ---
OS_EVENT *vga_trigger_sem; // Semaphore to wake up the VGA task

//base addresses
int* DRAM_WRADDR = (int*) 0x01E00000;
int* ACCE_WRADDR = (int*) 0x01E04B00;
int* EMER_ADDR = (int*) 0x01E02580;
int* CORE2_WRADDR = (int*) 0x01E04B10;

volatile int edge_capture;
volatile int start_vga = 0;
volatile int rx_buffer;
volatile int emergency_stop = 0;
volatile int prev_emergency = 0;

static void init_rx();
static void handle_rx_interrupts(void* context, alt_u32 id);

static void init_rx() {
    void* edge_capture_ptr = (void*) &edge_capture;
    IOWR_ALTERA_AVALON_PIO_IRQ_MASK(IRQ_CORE2_RX_BASE, 0xFFFFFFFF);
    IOWR_ALTERA_AVALON_PIO_EDGE_CAP(IRQ_CORE2_RX_BASE, 0);
    alt_irq_register( IRQ_CORE2_RX_IRQ,edge_capture_ptr,handle_rx_interrupts);
}

static void handle_rx_interrupts (void* context, alt_u32 id) {
    volatile int* edge_capture_ptr = (volatile int*) context;

    int edge = IORD_ALTERA_AVALON_PIO_EDGE_CAP(IRQ_CORE2_RX_BASE);
    if(edge & 0x00000001) emergency_stop = 1;
    if(edge & 0x00000002) emergency_stop = 0;

    *edge_capture_ptr = edge;
    rx_buffer = edge;

    // Clear the interrupt
    IOWR_ALTERA_AVALON_PIO_EDGE_CAP(IRQ_CORE2_RX_BASE, edge);

    // --- RTOS FIX: Wake up the VGA task! ---
    OSSemPost(vga_trigger_sem);
}

#include "altera_avalon_pio_regs.h" // Required for the PIO macros
#include "system.h"                 // Required for your BASE addresses

void write_pixel_8bit(uint8_t* ADDRESS) {
    unsigned int addr = 0;

    // Loop 9600 times (once for every byte in your array)
    for (int i = 0; i < 9600; i++) {
        // Read exactly 1 byte directly from memory
        uint8_t byte_pixel = IORD_8DIRECT(ADDRESS, i);

        // Loop 8 times (for the 8 bits in this byte)
        for (int b = 0; b < 8; b++) {
            // Check the top bit (0x80) instead of 0x80000000
            uint8_t pixel_val = (byte_pixel & 0x80) ? 0x0F : 0x00;

            IOWR_ALTERA_AVALON_PIO_DATA(IMG_ADDR_BASE, addr++);
            IOWR_ALTERA_AVALON_PIO_DATA(PIXEL_BASE, pixel_val);

            IOWR_ALTERA_AVALON_PIO_DATA(WREN_BASE, 1);
            IOWR_ALTERA_AVALON_PIO_DATA(WREN_BASE, 0);

            byte_pixel <<= 1;
        }
    }
    alt_printf("Image display complete!\n");
    start_vga = 0;
}

void write_pixel(int* ADDRESS) {
    uint32_t offset_read = 0;
    unsigned int addr = 0;

    for (int i = 0; i < 2400; i++) {
        // We still use IORD_32DIRECT here because we are reading from
        // raw SDRAM memory, not a PIO component.
        uint32_t word_pixel = IORD_32DIRECT(ADDRESS, offset_read);
        offset_read += 4;

        for (int b = 0; b < 32; b++) {
            // Determine pixel color (0x0F for white/on, 0x00 for black/off)
            uint8_t pixel_val = (word_pixel & 0x80000000) ? 0x0F : 0x00;

            // 1. Write the Address to the hardware
            IOWR_ALTERA_AVALON_PIO_DATA(IMG_ADDR_BASE, addr++);

            // 2. Write the Pixel Data to the hardware
            IOWR_ALTERA_AVALON_PIO_DATA(PIXEL_BASE, pixel_val);

            // 3. Pulse Write Enable (Cache Bypassed - hardware WILL see this!)
            IOWR_ALTERA_AVALON_PIO_DATA(WREN_BASE, 1);
            IOWR_ALTERA_AVALON_PIO_DATA(WREN_BASE, 0);

            word_pixel <<= 1;
        }
    }
    alt_printf("Image display complete!\n");
    start_vga = 0;
}

/* Definition of Task Stacks */
#define   TASK_STACKSIZE       1024
OS_STK    vga_task_stk[TASK_STACKSIZE];
OS_STK startup_task_stk[TASK_STACKSIZE];
OS_STK    cpu_monitor_task_stk[TASK_STACKSIZE];

/* Definition of Task Priorities */
#define VGA_PRIORITY      1
#define CPU_MONITOR_PRIO  2

void vga_task(void* pdata)
{
    INT8U err; // Variable to hold RTOS error codes

    while(1){
        // Pend (Sleep) until the ISR triggers the semaphore ---
        // '0' means wait forever with no timeout.
        // CPU usage drops to 0% here until the ISR calls OSSemPost!
        OSSemPend(vga_trigger_sem, 0, &err);

        if(!emergency_stop){
            if(rx_buffer & 0x80000000){
                write_pixel(DRAM_WRADDR);
                rx_buffer = 0x0;
            }
        }
        if(!prev_emergency && emergency_stop){
        	write_pixel_8bit((uint8_t*) emergency_image);
        }
        prev_emergency = emergency_stop;
    }
}

void cpu_monitor_task(void* pdata) {
    while(1) {
        // Write the CPU usage percentage (0-100) to your exact SDRAM address
        IOWR_32DIRECT(CORE2_WRADDR, 0, OSCPUUsage);

        // Sleep for 500 milliseconds, then update it again
        OSTimeDlyHMSM(0, 0, 0, 500);
    }
}

void startup_task(void* pdata) {
    // 1. Initialize the stats (calculates what 0% CPU looks like)
    OSStatInit();

    // 2. Create the VGA task
    OSTaskCreateExt(vga_task,
                  NULL,
                  (void *)&vga_task_stk[TASK_STACKSIZE-1],
                  VGA_PRIORITY,
                  VGA_PRIORITY,
                  vga_task_stk,
                  TASK_STACKSIZE,
                  NULL,
                  0);

    // 3. Create the CPU Monitor task
    OSTaskCreateExt(cpu_monitor_task,
                  NULL,
                  (void *)&cpu_monitor_task_stk[TASK_STACKSIZE-1],
                  CPU_MONITOR_PRIO,
                  CPU_MONITOR_PRIO,
                  cpu_monitor_task_stk,
                  TASK_STACKSIZE,
                  NULL,
                  0);

    // 4. Delete this startup task, its job is done!
    OSTaskDel(OS_PRIO_SELF);
}

int main(void)
{
    alt_printf("=========================================================\n");
    alt_printf("CPU Core 2 Alive\n");
    alt_printf("=========================================================\n");

    // --- RTOS FIX: Create the semaphore, initialized to 0 ---
    vga_trigger_sem = OSSemCreate(0);

    init_rx();

    OSTaskCreateExt(startup_task, NULL, (void *)&startup_task_stk[TASK_STACKSIZE-1], 0, 0, startup_task_stk, TASK_STACKSIZE, NULL, 0);

    OSStart();
    return 0;
}
