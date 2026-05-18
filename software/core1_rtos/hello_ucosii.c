// core 1
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
#include "unistd.h"
#include "includes.h"

// --- RTOS GLOBALS ---
OS_EVENT *accel_sem;

// Base addresses
// Image occupies 0x01E00000 → 0x01E12BFF (76800 bytes)
int* DRAM_WRADDR  = (int*) 0x01E00000;
int* ACCE_WRADDR  = (int*) 0x01E12C00;
int* CORE0_WRADDR = (int*) 0x01E12C0C;
int* CORE2_WRADDR = (int*) 0x01E12C10;
#define SPI_BASE 0x84001040

// Interrupt global variables
volatile int edge_capture   = 0;
volatile int start_spi      = 0;
volatile int emergency_stop = 0;
alt_up_accelerometer_spi_dev *accel;
volatile int scroll_flag    = 0;
volatile int video_mode     = 0;

// Function prototypes
void display_6chars(char* msg, int offset, int len);
unsigned char get_seg7(char c);
void play_tone(int freq_hz, int duration_ms);

// 7-segment arrays
unsigned char seg7_alpha[]   = {0x88,0x83,0xC6,0xA1,0x86,0x8E,0xC2,0x89,0xF9,0xE1,0x89,0xC7,0xC8,0xAB,0xC0,0x8C,0x98,0xAF,0x92,0x87,0xC1,0xC1,0xC1,0x89,0x91,0xA4,0xFF};
unsigned char seg7_numbers[] = {0xC0,0xF9,0xA4,0xB0,0x99,0x92,0x82,0xF8,0x80,0x90};

// --- INTERRUPT SERVICE ROUTINES ---

static void handle_rx_interrupts(void* context, alt_u32 id) {
    int edge = IORD_ALTERA_AVALON_PIO_EDGE_CAP(IRQ_CORE1_RX_BASE);
    if (edge & 0x00010000) emergency_stop = 1;
    if (edge & 0x00020000) emergency_stop = 0;
    IOWR_ALTERA_AVALON_PIO_EDGE_CAP(IRQ_CORE1_RX_BASE, edge);
}

static void handle_key_interrupts(void* context, alt_u32 id) {
    int edge = IORD_ALTERA_AVALON_PIO_EDGE_CAP(PUSH_BUTTONS_BASE);
    if (edge & 0x1) start_spi = !start_spi;
    if (edge & 0x2) {
        IOWR_ALTERA_AVALON_PIO_DATA(IRQ_CORE1_TX_BASE, 0x80000000);
        IOWR_ALTERA_AVALON_PIO_DATA(IRQ_CORE1_TX_BASE, 0x00000000);
    }
    IOWR_ALTERA_AVALON_PIO_EDGE_CAP(PUSH_BUTTONS_BASE, 0);
}

static void handle_timer_interrupts(void* context, alt_u32 id) {
    int edge = IORD_ALTERA_AVALON_PIO_EDGE_CAP(CLOCK_INTERRUPT_BASE);
    if (edge & 0x1) scroll_flag = 1;
    if (edge & 0x2) OSSemPost(accel_sem);
    IOWR_ALTERA_AVALON_PIO_EDGE_CAP(CLOCK_INTERRUPT_BASE, 0);
}

// --- INIT FUNCTIONS ---
static void init_rx() {
    IOWR_ALTERA_AVALON_PIO_IRQ_MASK(IRQ_CORE1_RX_BASE, 0xFFFFFFFF);
    IOWR_ALTERA_AVALON_PIO_EDGE_CAP(IRQ_CORE1_RX_BASE, 0);
    alt_irq_register(IRQ_CORE1_RX_IRQ, (void*)&edge_capture, handle_rx_interrupts);
}

static void init_key_pio() {
    IOWR_ALTERA_AVALON_PIO_IRQ_MASK(PUSH_BUTTONS_BASE, 0x3);
    IOWR_ALTERA_AVALON_PIO_EDGE_CAP(PUSH_BUTTONS_BASE, 0);
    alt_irq_register(PUSH_BUTTONS_IRQ, (void*)&edge_capture, handle_key_interrupts);
}

static void init_timer() {
    IOWR_ALTERA_AVALON_PIO_IRQ_MASK(CLOCK_INTERRUPT_BASE, 0x3);
    IOWR_ALTERA_AVALON_PIO_EDGE_CAP(CLOCK_INTERRUPT_BASE, 0);
    alt_irq_register(CLOCK_INTERRUPT_IRQ, NULL, handle_timer_interrupts);
}

// --- HARDWARE HELPERS ---
unsigned char get_seg7(char c) {
    if (c >= 'A' && c <= 'Z') return seg7_alpha[c - 'A'];
    if (c >= 'a' && c <= 'z') return seg7_alpha[c - 'a'];
    if (c >= '0' && c <= '9') return seg7_numbers[c - '0'];
    if (c == ' ') return 0xFF;
    if (c == '-') return 0xBF;
    if (c == '.') return 0x7F;
    if (c == '_') return 0xE7;
    if (c == '?') return 0x86;
    return 0xFF;
}

void display_6chars(char* msg, int offset, int len) {
    char chars[6];
    for (int i = 0; i < 6; i++) chars[i] = msg[(offset + i) % len];
    unsigned int hex012 = (get_seg7(chars[3]) << 16) | (get_seg7(chars[4]) << 8) | get_seg7(chars[5]);
    unsigned int hex345 = (get_seg7(chars[0]) << 16) | (get_seg7(chars[1]) << 8) | get_seg7(chars[2]);
    IOWR_ALTERA_AVALON_PIO_DATA(HEX012_BASE, hex012);
    IOWR_ALTERA_AVALON_PIO_DATA(HEX345_BASE, hex345);
}

void play_tone(int freq_hz, int duration_ms) {
    int half_period_us = 1000000 / (freq_hz * 2);
    int cycles = (duration_ms * freq_hz) / 1000;
    for (int i = 0; i < cycles; i++) {
        IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, 1); usleep(half_period_us);
        IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, 0); usleep(half_period_us);
    }
}

// --- TASK 1: CAMERA SPI ---
#define PAYLOAD_BYTES 60
#define SPI_WORDS ((PAYLOAD_BYTES + 4) / 4)  // 16 words

int bad_frames = 0;

void camera_spi_task(void* pdata) {
    uint8_t pattern_hits = 0;
    int offset_write = 0;
    int write_len    = 0;
    uint32_t buffer[SPI_WORDS];
    int trigger_sent = 0;

    while (1) {
        if (start_spi) {
            video_mode = 1;

            if (trigger_sent == 0) {
                bad_frames = 0;
                IOWR_ALTERA_AVALON_SPI_SLAVE_SEL(SPI_BASE, 0x1);
                IOWR_ALTERA_AVALON_SPI_TXDATA(SPI_BASE, 0x01);
                while (!(IORD_ALTERA_AVALON_SPI_STATUS(SPI_BASE) & 0x80));
                IORD_ALTERA_AVALON_SPI_RXDATA(SPI_BASE);
                IOWR_ALTERA_AVALON_SPI_SLAVE_SEL(SPI_BASE, 0x0);
                trigger_sent = 1;
                OSTimeDlyHMSM(0, 0, 0, 2);
            }

            OSSchedLock();
            IOWR_ALTERA_AVALON_SPI_CONTROL(SPI_BASE, 0x400);
            IOWR_ALTERA_AVALON_SPI_SLAVE_SEL(SPI_BASE, 0x1);
            for (uint8_t buffer_i = 0; buffer_i < SPI_WORDS; buffer_i++) {
                while (!(IORD_ALTERA_AVALON_SPI_STATUS(SPI_BASE) & 0x40));
                IOWR_ALTERA_AVALON_SPI_TXDATA(SPI_BASE, 0x00);
                while (!(IORD_ALTERA_AVALON_SPI_STATUS(SPI_BASE) & 0x80));
                buffer[buffer_i] = IORD_ALTERA_AVALON_SPI_RXDATA(SPI_BASE);
            }
            IOWR_ALTERA_AVALON_SPI_SLAVE_SEL(SPI_BASE, 0x0);
            IOWR_ALTERA_AVALON_SPI_CONTROL(SPI_BASE, 0x000);
            OSSchedUnlock();

            if (buffer[0] == 0xFF00FF00 && buffer[1] == 0xFF00FF00) {
                if (pattern_hits == 0) {
                    pattern_hits = 1;
                    offset_write = 0;
                    write_len    = 0;
                } else {
                    if (write_len < 69120) {
                        // False stop — next frame starting
                        pattern_hits = 1;
                        offset_write = 0;
                        write_len    = 0;
                        bad_frames++;
                    } else {
                        pattern_hits = 0;
                        offset_write = 0;

                        if (write_len == 76800) {
                            bad_frames = 0;
                            IOWR_ALTERA_AVALON_PIO_DATA(ONBOARD_LEDS_BASE, 0x200);
                            IOWR_ALTERA_AVALON_PIO_DATA(IRQ_CORE1_TX_BASE, 0x80000000);
                            IOWR_ALTERA_AVALON_PIO_DATA(IRQ_CORE1_TX_BASE, 0x00000000);
                        } else {
                            bad_frames++;
                            IOWR_ALTERA_AVALON_PIO_DATA(ONBOARD_LEDS_BASE, 0x100);
                        }

                        write_len = 0;

                        if (bad_frames >= 5) {
                            start_spi  = 0;
                            video_mode = 0;
                            bad_frames = 0;
                            trigger_sent = 0;
                            continue;
                        }

                        if (start_spi) trigger_sent = 0;
                    }
                }
            } else if (pattern_hits == 1) {
                if ((buffer[0] >> 16) == 0xA0A0 &&
                    (buffer[SPI_WORDS-1] & 0xFFFF) == 0xA0A0) {
                    int payload_words = PAYLOAD_BYTES / 4;
                    for (int i = 0; i < payload_words; i++) {
                        uint32_t word = (buffer[i] << 16) | (buffer[i+1] >> 16);
                        IOWR_32DIRECT(DRAM_WRADDR, offset_write, word);
                        offset_write += 4;
                        write_len    += 4;
                    }
                }
            }

            OSTimeDlyHMSM(0, 0, 0, 1);

        } else {
            video_mode   = 0;
            trigger_sent = 0;
            OSTimeDlyHMSM(0, 0, 0, 2);
        }
    }
}

// --- TASK 2: ACCELEROMETER ---
void accel_task(void* pdata) {
    INT8U err;
    alt_32 x, y, z;
    int emergency_led = 0;

    while (1) {
        OSSemPend(accel_sem, 0, &err);

        alt_up_accelerometer_spi_read_x_axis(accel, &x);
        alt_up_accelerometer_spi_read_y_axis(accel, &y);
        alt_up_accelerometer_spi_read_z_axis(accel, &z);

        IOWR_32DIRECT(ACCE_WRADDR, 0, (uint32_t)x);
        IOWR_32DIRECT(ACCE_WRADDR, 4, (uint32_t)y);
        IOWR_32DIRECT(ACCE_WRADDR, 8, (uint32_t)z);

        // Don't signal Core 2 during video — would corrupt VGA task timing
        if (!video_mode) {
            IOWR_ALTERA_AVALON_PIO_DATA(IRQ_CORE1_TX_BASE, 0x00008000);
            IOWR_ALTERA_AVALON_PIO_DATA(IRQ_CORE1_TX_BASE, 0x00000000);
        }

        if (z < -208) {
            IOWR_ALTERA_AVALON_PIO_DATA(ONBOARD_LEDS_BASE, emergency_led ? 0x3FF : 0x000);
            emergency_led = !emergency_led;
        }
    }
}

// --- TASK 3: UI ---
void ui_task(void* pdata) {
    int   scroll_offset  = 0;
    char* message        = "ARIGA-67  ";
    int   len            = strlen(message);
    char  cpu_message[7] = "C0-000";
    uint8_t cpu_idx      = 0;

    while (1) {
        if (!scroll_flag) { OSTimeDly(1); continue; }
        scroll_flag = 0;

        // Removed: if (video_mode) continue;
        // UI runs freely — 7-seg and switches don't touch SPI hardware

        if (!emergency_stop) {
            int sw   = IORD_ALTERA_AVALON_PIO_DATA(SWITCHES_BASE);
            int sw1  = (sw >> 0) & 0x1;
            int sw2  = (sw >> 1) & 0x1;
            int sw3  = (sw >> 2) & 0x1;
            int sw4  = (sw >> 3) & 0x1;
            int sw10 = (sw >> 9) & 0x1;

            if (!sw1) {
                scroll_offset = 0;
                IOWR_ALTERA_AVALON_PIO_DATA(HEX012_BASE, 0xFFFFFF);
                IOWR_ALTERA_AVALON_PIO_DATA(HEX345_BASE, 0xFFFFFF);
            } else if (sw1 && sw2) {
                if (sw3) {
                    int core_state = cpu_idx / 4;
                    if (core_state == 0) {
                        int v = IORD_32DIRECT(CORE0_WRADDR, 0);
                        snprintf(cpu_message, 7, "C0-%03d", v);
                    } else if (core_state == 1) {
                        snprintf(cpu_message, 7, "C1-%03d", OSCPUUsage);
                    } else if (core_state == 2) {
                        int v = IORD_32DIRECT(CORE2_WRADDR, 0);
                        snprintf(cpu_message, 7, "C2-%03d", v);
                    }
                    display_6chars(cpu_message, 0, strlen(cpu_message));
                    if (++cpu_idx >= 12) cpu_idx = 0;
                } else {
                    display_6chars(message, scroll_offset, len);
                    scroll_offset = (scroll_offset + 1) % len;
                }
            } else {
                display_6chars(message, scroll_offset, len);
            }

            if (sw4)  play_tone(440, 500);
            if (sw10) IOWR_ALTERA_AVALON_PIO_DATA(ONBOARD_LEDS_BASE, 0x0);
        }
    }
}

// --- MAIN ---
#define TASK_STACKSIZE 1024
OS_STK camera_task_stk [TASK_STACKSIZE];
OS_STK accel_task_stk  [TASK_STACKSIZE];
OS_STK ui_task_stk     [TASK_STACKSIZE];
OS_STK startup_task_stk[TASK_STACKSIZE];

#define CAMERA_PRIORITY 1
#define ACCEL_PRIORITY  2
#define UI_PRIORITY     3

void startup_task(void* pdata) {
    OSStatInit();
    OSTaskCreateExt(camera_spi_task, NULL, (void*)&camera_task_stk [TASK_STACKSIZE-1], CAMERA_PRIORITY, CAMERA_PRIORITY, camera_task_stk,  TASK_STACKSIZE, NULL, 0);
    OSTaskCreateExt(accel_task,      NULL, (void*)&accel_task_stk  [TASK_STACKSIZE-1], ACCEL_PRIORITY,  ACCEL_PRIORITY,  accel_task_stk,   TASK_STACKSIZE, NULL, 0);
    OSTaskCreateExt(ui_task,         NULL, (void*)&ui_task_stk     [TASK_STACKSIZE-1], UI_PRIORITY,     UI_PRIORITY,     ui_task_stk,      TASK_STACKSIZE, NULL, 0);
    OSTaskDel(OS_PRIO_SELF);
}

int main(void) {
    alt_printf("=========================================================\n");
    alt_printf("CPU Core 1 Alive\n");
    alt_printf("=========================================================\n");

    accel = alt_up_accelerometer_spi_open_dev("/dev/accelerometer_spi_0");
    if (!accel) { alt_printf("Failed to open accelerometer\n"); return 1; }
    alt_printf("Accelerator initialized\n");

    IOWR_ALTERA_AVALON_PIO_DATA(ONBOARD_LEDS_BASE, 0x0);
    accel_sem = OSSemCreate(0);

    init_rx();
    init_key_pio();
    init_timer();

    OSTaskCreateExt(startup_task, NULL, (void*)&startup_task_stk[TASK_STACKSIZE-1], 0, 0, startup_task_stk, TASK_STACKSIZE, NULL, 0);
    OSStart();
    return 0;
}
