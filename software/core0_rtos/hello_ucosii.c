//core 0
#include <math.h>
#include "system.h"
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
#include <stdio.h>
#include "includes.h"

// --- RTOS GLOBALS ---
OS_EVENT *rx_trigger_sem;

volatile int rx_buffer      = 0;
volatile int edge_capture   = 0;
volatile int emergency_stop = 0;
volatile int cube_mode      = 0;

static void init_rx();
static void handle_rx_interrupts(void* context, alt_u32 id);

// --- MEMORY MAP ---
int* EMER_ADDR    = (int*) 0x01E12C14;
int* ACCE_WRADDR  = (int*) 0x01E12C00;
int* CORE0_WRADDR = (int*) 0x01E12C0C;

// --- RENDER CONSTANTS ---
#define LENGTH 320
#define WIDTH  240

static float cube_size   = 15.0f;
static float scale_const = 40.0f;
static float focal       = 100.0f;
static float light_x = 0.0f, light_y = 0.0f, light_z = 1.0f;

static float*   buffer_z  = (float*)   0x01E25C00;
static uint8_t* buffer_2d = (uint8_t*) 0x01E00000;

// Store last known accel values so cube can re-render immediately on mode toggle
static alt_32 last_ax = 0, last_ay = 0;

// ---------------------------------------------------------------------------
// Render with PRECOMPUTED rotation matrix — trig called only 6 times per frame
// instead of ~6 times per pixel (massive speedup)
// ---------------------------------------------------------------------------
static void render_cube(alt_32 ax, alt_32 ay) {
	OSTimeDly(3);

    last_ax = ax;
    last_ay = ay;

    float X = (float)ax / 512.0f * 3.14159f;
    float Y = (float)ay / 512.0f * 3.14159f;

    // Precompute rotation matrix ONCE for the whole frame
    float cX = cosf(X), sX = sinf(X);
    float cY = cosf(Y), sY = sinf(Y);
    // Z=0: cosZ=1, sinZ=0
    // Full rotation matrix R = Rx * Ry * Rz (Z=0 simplifies)
    float r00 =  cX*cY;
    float r01 = -sX;
    float r02 =  cX*sY;
    float r10 =  sX*cY;
    float r11 =  cX;
    float r12 =  sX*sY;
    float r20 = -sY;
    float r21 =  0.0f;
    float r22 =  cY;

    // Clear buffers
    for (int i = 0; i < LENGTH * WIDTH; i++) {
        IOWR_8DIRECT (buffer_2d, i,     0);
        IOWR_32DIRECT(buffer_z,  i * 4, 0);
    }

    float cs = cube_size;

    for (float cx = -cs; cx < cs; cx++) {
        for (float cy = -cs; cy < cs; cy++) {
            for (float cz = -cs; cz < cs; cz++) {

                // Only render surface pixels
                int on_surface = (cx==-cs || cx==cs-1 ||
                                  cy==-cs || cy==cs-1 ||
                                  cz==-cs || cz==cs-1);
                if (!on_surface) continue;

                // Rotate point using precomputed matrix
                float rx = r00*cx + r01*cy + r02*cz;
                float ry = r10*cx + r11*cy + r12*cz;
                float rz = r20*cx + r21*cy + r22*cz + focal;
                if (rz <= 0) continue;

                // Surface normal
                float nx = (cx==cs-1)?1.0f:(cx==-cs)?-1.0f:0.0f;
                float ny = (cy==cs-1)?1.0f:(cy==-cs)?-1.0f:0.0f;
                float nz = (cz==cs-1)?1.0f:(cz==-cs)?-1.0f:0.0f;
                // Replace the sqrtf block with this high-speed alternative:
                float sum_sq = nx*nx + ny*ny + nz*nz;
                if (sum_sq > 0.0f) {
                    float mag;
                    if (sum_sq == 1.0f)      mag = 1.0f;
                    else if (sum_sq == 2.0f) mag = 1.414213f; // Pre-calculated sqrt(2)
                    else                     mag = 1.732051f; // Pre-calculated sqrt(3)

                    nx /= mag;
                    ny /= mag;
                    nz /= mag;
                }

                // Rotate normal using same precomputed matrix
                float rnx = r00*nx + r01*ny + r02*nz;
                float rny = r10*nx + r11*ny + r12*nz;
                float rnz = r20*nx + r21*ny + r22*nz;

                // Lighting
                float ooz   = 1.0f / rz;
                float bright = rnx*light_x + rny*light_y + rnz*light_z;
                if (bright < 0) bright = 0;
                if (bright > 1) bright = 1;

                // Map to RGB332 grayscale
                uint8_t s    = (uint8_t)(bright * 7);
                uint8_t gray = (s << 5) | (s << 2) | (s >> 1);

                int xpx = (int)(LENGTH/2 + scale_const * rx * ooz * 2);
                int ypx = (int)(WIDTH/2  + scale_const * ry * ooz);

                if (xpx < 0 || xpx >= LENGTH || ypx < 0 || ypx >= WIDTH) continue;

                int idx = xpx + LENGTH * ypx;
                uint32_t cur_z_raw = IORD_32DIRECT(buffer_z, idx * 4);
                float cur_z_f = *(float*)&cur_z_raw;
                if (ooz > cur_z_f) {
                    IOWR_32DIRECT(buffer_z,  idx * 4, *(uint32_t*)&ooz);
                    IOWR_8DIRECT (buffer_2d, idx,     gray);
                }
            }
        }
    }
    // Signal Core 2 to display cube frame
    IOWR_ALTERA_AVALON_PIO_DATA(IRQ_CORE0_TX_BASE, 0x00000004);
    IOWR_ALTERA_AVALON_PIO_DATA(IRQ_CORE0_TX_BASE, 0x00000000);
}

// ---------------------------------------------------------------------------
// ISR
// ---------------------------------------------------------------------------
static void handle_rx_interrupts(void* context, alt_u32 id) {
    volatile int* edge_capture_ptr = (volatile int*) context;

    int edge = IORD_ALTERA_AVALON_PIO_EDGE_CAP(IRQ_CORE0_RX_BASE);
    *edge_capture_ptr = edge;
    rx_buffer = edge;
    IOWR_ALTERA_AVALON_PIO_EDGE_CAP(IRQ_CORE0_RX_BASE, edge);

    // Toggle cube mode when Core 1 sends 0x00007000
    if (edge & 0x00007000) {

        cube_mode = !cube_mode;
        // If cube was just enabled, mark rx_buffer as accel update
        // so accel_task renders immediately using last known values
        if (cube_mode) {
            rx_buffer = 0x00008000;
        }
        alt_printf("%x\n",cube_mode);
    }

    OSSemPost(rx_trigger_sem);
}

static void init_rx() {
    void* edge_capture_ptr = (void*) &edge_capture;
    IOWR_ALTERA_AVALON_PIO_IRQ_MASK(IRQ_CORE0_RX_BASE, 0xFFFFFFFF);
    IOWR_ALTERA_AVALON_PIO_EDGE_CAP(IRQ_CORE0_RX_BASE, 0);
    alt_irq_register(IRQ_CORE0_RX_IRQ, edge_capture_ptr, handle_rx_interrupts);
}

// ---------------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------------
#define TASK_STACKSIZE 1024
OS_STK accel_task_stk  [TASK_STACKSIZE];
OS_STK startup_task_stk[TASK_STACKSIZE];

#define ACCEL_PRIORITY 1

void accel_task(void* pdata) {
    INT8U err;
    while (1) {
        OSSemPend(rx_trigger_sem, 0, &err);

        if (rx_buffer & 0x00008000) {
            int32_t received_x = (int32_t) IORD_32DIRECT(ACCE_WRADDR, 0);
            int32_t received_y = (int32_t) IORD_32DIRECT(ACCE_WRADDR, 4);
            int32_t received_z = (int32_t) IORD_32DIRECT(ACCE_WRADDR, 8);
            rx_buffer = 0x0;

            // Emergency stop
            if (received_z < -208 && !emergency_stop) {
                IOWR_ALTERA_AVALON_PIO_DATA(IRQ_CORE0_TX_BASE, 0x00010001);
                IOWR_ALTERA_AVALON_PIO_DATA(IRQ_CORE0_TX_BASE, 0x00000000);
                emergency_stop = 1;
                alt_printf("DEVICE UPSIDE DOWN\n");
            } else if (received_z > -208 && emergency_stop) {
                IOWR_ALTERA_AVALON_PIO_DATA(IRQ_CORE0_TX_BASE, 0x00020002);
                IOWR_ALTERA_AVALON_PIO_DATA(IRQ_CORE0_TX_BASE, 0x00000000);
                emergency_stop = 0;
                alt_printf("DEVICE BACK ALIVE\n");
            }

            IOWR_32DIRECT(CORE0_WRADDR, 0, OSCPUUsage);

            // Render cube if enabled
            if (cube_mode) {
                // Use fresh accel values if available, otherwise last known
                alt_32 ax = (received_x != 0 || last_ax == 0) ? received_x : last_ax;
                alt_32 ay = (received_y != 0 || last_ay == 0) ? received_y : last_ay;
                render_cube(ax, ay);
            }
        }
    }
}

void startup_task(void* pdata) {
    OSStatInit();
    OSTaskCreateExt(accel_task, NULL, (void*)&accel_task_stk[TASK_STACKSIZE-1],
                    ACCEL_PRIORITY, ACCEL_PRIORITY, accel_task_stk, TASK_STACKSIZE, NULL, 0);
    OSTaskDel(OS_PRIO_SELF);
}

int main(void) {
    alt_printf("=========================================================\n");
    alt_printf("CPU Core 0 Alive\n");
    alt_printf("=========================================================\n");


    rx_trigger_sem = OSSemCreate(0);
    init_rx();

    OSTaskCreateExt(startup_task, NULL, (void*)&startup_task_stk[TASK_STACKSIZE-1],
                    0, 0, startup_task_stk, TASK_STACKSIZE, NULL, 0);
    OSStart();
    return 0;
}
