// core 2
#include <image.h>
#include <emergencyIMG2.h>
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

// --- RTOS GLOBALS ---
OS_EVENT *vga_trigger_sem;

// --- MEMORY MAP ---
int* DRAM_WRADDR  = (int*) 0x01E00000;
int* ACCE_WRADDR  = (int*) 0x01E12C00;
int* EMER_ADDR    = (int*) 0x01E12C14;
int* CORE2_WRADDR = (int*) 0x01E12C10;

volatile int edge_capture   = 0;
volatile int rx_buffer      = 0;
volatile int emergency_stop = 0;
volatile int emergency_counter = 0;
volatile int is_cube = 0;

static void init_rx();
static void handle_rx_interrupts(void* context, alt_u32 id);

static void init_rx() {
    void* edge_capture_ptr = (void*) &edge_capture;
    IOWR_ALTERA_AVALON_PIO_IRQ_MASK(IRQ_CORE2_RX_BASE, 0xFFFFFFFF);
    IOWR_ALTERA_AVALON_PIO_EDGE_CAP(IRQ_CORE2_RX_BASE, 0);
    alt_irq_register(IRQ_CORE2_RX_IRQ, edge_capture_ptr, handle_rx_interrupts);
}

static void handle_rx_interrupts(void* context, alt_u32 id) {
    volatile int* edge_capture_ptr = (volatile int*) context;

    int edge = IORD_ALTERA_AVALON_PIO_EDGE_CAP(IRQ_CORE2_RX_BASE);

    if (edge & 0x00000001) emergency_stop = 1;
    if (edge & 0x00000002) emergency_stop = 0;

    *edge_capture_ptr = edge;

    // Camera frame ready
    if (edge & 0x80000000) {
        rx_buffer |= 0x80000000;
        OSSemPost(vga_trigger_sem);
    }
    // Cube frame ready
    if (edge & 0x00000004) {
        rx_buffer |= 0x00000004;
        OSSemPost(vga_trigger_sem);
    }

    // Emergency signals — wake VGA to show emergency image
    else if (edge & 0x00000003) {
        OSSemPost(vga_trigger_sem);
    }

    IOWR_ALTERA_AVALON_PIO_EDGE_CAP(IRQ_CORE2_RX_BASE, edge);
}

// ---------------------------------------------------------------------------
// Display functions
// ---------------------------------------------------------------------------

// Camera frame — has endian swap because Core 1 packs via 32-bit SPI words
void write_pixel_color(int* ADDRESS) {
    int img_addr = 0;

    // 76800 pixels / 4 pixels per read = 19200 iterations
    for (int i = 0; i < 19200; i++) {
        // 1. Read 4 pixel bytes at once (offset in bytes = i * 4)
        uint32_t word = IORD_32DIRECT(ADDRESS, i * 4);

        // 2. Process 4 pixels sequentially.
        // Little-endian layout naturally aligns the bytes to be read from MSB to LSB.

        // Pixel 0 (Originally i, byte_addr = 3)
        IOWR_ALTERA_AVALON_PIO_DATA(IMG_ADDR_BASE, img_addr++);
        IOWR_ALTERA_AVALON_PIO_DATA(PIXEL_BASE, (uint8_t)(word >> 24));
        IOWR_ALTERA_AVALON_PIO_DATA(WREN_BASE, 1);
        IOWR_ALTERA_AVALON_PIO_DATA(WREN_BASE, 0);

        // Pixel 1 (Originally i+1, byte_addr = 2)
        IOWR_ALTERA_AVALON_PIO_DATA(IMG_ADDR_BASE, img_addr++);
        IOWR_ALTERA_AVALON_PIO_DATA(PIXEL_BASE, (uint8_t)(word >> 16));
        IOWR_ALTERA_AVALON_PIO_DATA(WREN_BASE, 1);
        IOWR_ALTERA_AVALON_PIO_DATA(WREN_BASE, 0);

        // Pixel 2 (Originally i+2, byte_addr = 1)
        IOWR_ALTERA_AVALON_PIO_DATA(IMG_ADDR_BASE, img_addr++);
        IOWR_ALTERA_AVALON_PIO_DATA(PIXEL_BASE, (uint8_t)(word >> 8));
        IOWR_ALTERA_AVALON_PIO_DATA(WREN_BASE, 1);
        IOWR_ALTERA_AVALON_PIO_DATA(WREN_BASE, 0);

        // Pixel 3 (Originally i+3, byte_addr = 0)
        IOWR_ALTERA_AVALON_PIO_DATA(IMG_ADDR_BASE, img_addr++);
        IOWR_ALTERA_AVALON_PIO_DATA(PIXEL_BASE, (uint8_t)word);
        IOWR_ALTERA_AVALON_PIO_DATA(WREN_BASE, 1);
        IOWR_ALTERA_AVALON_PIO_DATA(WREN_BASE, 0);
    }
}

// Cube frame — no endian swap, Core 0 writes bytes directly
// Pixels are already grayscale RGB332 from render_cube()
void write_pixel_cube(int* ADDRESS) {
    for (int i = 0; i < 76800; i++) {
        uint8_t pixel = IORD_8DIRECT(ADDRESS, i);
        IOWR_ALTERA_AVALON_PIO_DATA(IMG_ADDR_BASE, i);
        IOWR_ALTERA_AVALON_PIO_DATA(PIXEL_BASE, pixel);
        IOWR_ALTERA_AVALON_PIO_DATA(WREN_BASE, 1);
        IOWR_ALTERA_AVALON_PIO_DATA(WREN_BASE, 0);
    }
}

void write_pixel_array(const uint8_t* ADDRESS) {
    for (int i = 0; i < 76800; i++) {
        IOWR_ALTERA_AVALON_PIO_DATA(IMG_ADDR_BASE, i);
        IOWR_ALTERA_AVALON_PIO_DATA(PIXEL_BASE, ADDRESS[i]);
        IOWR_ALTERA_AVALON_PIO_DATA(WREN_BASE, 1);
        IOWR_ALTERA_AVALON_PIO_DATA(WREN_BASE, 0);
    }
}
// ---------------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------------
#define TASK_STACKSIZE 1024
OS_STK vga_task_stk         [TASK_STACKSIZE];
OS_STK startup_task_stk     [TASK_STACKSIZE];
OS_STK cpu_monitor_task_stk [TASK_STACKSIZE];

#define VGA_PRIORITY      1
#define CPU_MONITOR_PRIO  2

void vga_task(void* pdata) {
    INT8U err;

    while (1) {
        // Wake on signal OR every 500ms — whichever comes first
        OSSemPend(vga_trigger_sem, OS_TICKS_PER_SEC / 2, &err);

        if (!emergency_stop) {
            emergency_counter = 0;

            if (rx_buffer & 0x00000004) {
                write_pixel_cube(DRAM_WRADDR);
                rx_buffer &= ~0x00000004;

            } else if (rx_buffer & 0x80000000) {
                write_pixel_color(DRAM_WRADDR);
                rx_buffer &= ~0x80000000;
            }
        }

        if (emergency_stop) {
            // Swap image every tick (500ms per image)
            const uint8_t* img = (emergency_counter % 2 == 0)
                                 ? (uint8_t*) emergency_image
                                 : (uint8_t*) emergencyIMG2;
            write_pixel_array(img);
            emergency_counter++;
        }
    }
}

void cpu_monitor_task(void* pdata) {
    while (1) {
        IOWR_32DIRECT(CORE2_WRADDR, 0, OSCPUUsage);
        OSTimeDlyHMSM(0, 0, 0, 500);
    }
}

void startup_task(void* pdata) {
    OSStatInit();
    OSTaskCreateExt(vga_task,         NULL, (void*)&vga_task_stk        [TASK_STACKSIZE-1], VGA_PRIORITY,     VGA_PRIORITY,     vga_task_stk,         TASK_STACKSIZE, NULL, 0);
    OSTaskCreateExt(cpu_monitor_task, NULL, (void*)&cpu_monitor_task_stk[TASK_STACKSIZE-1], CPU_MONITOR_PRIO, CPU_MONITOR_PRIO, cpu_monitor_task_stk, TASK_STACKSIZE, NULL, 0);
    OSTaskDel(OS_PRIO_SELF);
}

int main(void) {
    alt_printf("=========================================================\n");
    alt_printf("CPU Core 2 Alive\n");
    alt_printf("=========================================================\n");

    vga_trigger_sem = OSSemCreate(0);
    init_rx();

    OSTaskCreateExt(startup_task, NULL, (void*)&startup_task_stk[TASK_STACKSIZE-1], 0, 0, startup_task_stk, TASK_STACKSIZE, NULL, 0);
    OSStart();
    return 0;
}
