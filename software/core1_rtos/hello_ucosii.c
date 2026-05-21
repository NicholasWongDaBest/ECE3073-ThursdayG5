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
OS_EVENT *music_sem;

// --- MEMORY MAP ---
// Image buffer:    0x01E00000 → 0x01E12BFF (76800 bytes)
// Z-buffer:        0x01E25C00 → 0x01E72BFF (307200 bytes, Core 0 cube)
int* DRAM_WRADDR  = (int*) 0x01E00000;
int* ACCE_WRADDR  = (int*) 0x01E12C00;
int* CORE0_WRADDR = (int*) 0x01E12C0C;
int* CORE2_WRADDR = (int*) 0x01E12C10;
#define SPI_BASE 0x84001040

// --- INTERRUPT GLOBALS ---
volatile int edge_capture   = 0;
volatile int start_spi      = 0;
volatile int emergency_stop = 0;
volatile int scroll_flag    = 0;
volatile int video_mode     = 0;

alt_up_accelerometer_spi_dev *accel;

// --- FUNCTION PROTOTYPES ---
void display_6chars(char* msg, int offset, int len);
unsigned char get_seg7(char c);
void play_tone();

// --- 7-SEGMENT ARRAYS ---
unsigned char seg7_alpha[]   = {0x88,0x83,0xC6,0xA1,0x86,0x8E,0xC2,0x89,0xF9,0xE1,0x89,0xC7,0xC8,0xAB,0xC0,0x8C,0x98,0xAF,0x92,0x87,0xC1,0xC1,0xC1,0x89,0x91,0xA4,0xFF};
unsigned char seg7_numbers[] = {0xC0,0xF9,0xA4,0xB0,0x99,0x92,0x82,0xF8,0x80,0x90};

// --- ISRs ---
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

// --- NOTE DEFINES ---
#define NOTE_REST 0
#define NOTE_F3   1
#define NOTE_FS3  2
#define NOTE_G3   3
#define NOTE_GS3  4
#define NOTE_A3   5
#define NOTE_AS3  6
#define NOTE_B3   7
#define NOTE_C4   8
#define NOTE_CS4  9
#define NOTE_D4   10
#define NOTE_DS4  11
#define NOTE_E4   12
#define NOTE_F4   13
#define NOTE_FS4  14
#define NOTE_G4   15
#define NOTE_GS4  16
#define NOTE_A4   17
#define NOTE_AS4  18
#define NOTE_B4   19
#define NOTE_C5   20
#define NOTE_CS5  21
#define NOTE_D5   22
#define NOTE_DS5  23
#define NOTE_E5   24

void play_tone() {
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_G4);  OSTimeDlyHMSM(0,0,0,900);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_A4);  OSTimeDlyHMSM(0,0,0,150);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_AS4); OSTimeDlyHMSM(0,0,0,150);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_G4);  OSTimeDlyHMSM(0,0,0,600);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_F4);  OSTimeDlyHMSM(0,0,0,550);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_REST);OSTimeDlyHMSM(0,0,0,100);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_F4);  OSTimeDlyHMSM(0,0,0,550);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_D4);  OSTimeDlyHMSM(0,0,0,600);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_E4);  OSTimeDlyHMSM(0,0,1,200);

    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_G4);  OSTimeDlyHMSM(0,0,0,900);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_A4);  OSTimeDlyHMSM(0,0,0,150);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_AS4); OSTimeDlyHMSM(0,0,0,150);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_G4);  OSTimeDlyHMSM(0,0,0,600);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_F4);  OSTimeDlyHMSM(0,0,0,550);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_REST);OSTimeDlyHMSM(0,0,0,100);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_F4);  OSTimeDlyHMSM(0,0,0,550);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_AS4); OSTimeDlyHMSM(0,0,0,600);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_C5);  OSTimeDlyHMSM(0,0,1,200);

    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_G4);  OSTimeDlyHMSM(0,0,0,400);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_REST);OSTimeDlyHMSM(0,0,0,100);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_G4);  OSTimeDlyHMSM(0,0,0,400);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_REST);OSTimeDlyHMSM(0,0,0,100);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_G4);  OSTimeDlyHMSM(0,0,0,400);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_REST);OSTimeDlyHMSM(0,0,0,50);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_G4);  OSTimeDlyHMSM(0,0,0,150);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_A4);  OSTimeDlyHMSM(0,0,0,150);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_G4);  OSTimeDlyHMSM(0,0,0,150);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_AS4); OSTimeDlyHMSM(0,0,0,150);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_G4);  OSTimeDlyHMSM(0,0,0,150);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_A4);  OSTimeDlyHMSM(0,0,0,150);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_G4);  OSTimeDlyHMSM(0,0,0,150);

    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_G4);  OSTimeDlyHMSM(0,0,0,400);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_REST);OSTimeDlyHMSM(0,0,0,100);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_G4);  OSTimeDlyHMSM(0,0,0,400);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_G4);  OSTimeDlyHMSM(0,0,0,125);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_REST);OSTimeDlyHMSM(0,0,0,50);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_G4);  OSTimeDlyHMSM(0,0,0,125);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_AS4); OSTimeDlyHMSM(0,0,1,200);
    IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_REST);
}

// --- TASK 1: CAMERA SPI ---
#define PAYLOAD_BYTES 60
#define SPI_WORDS ((PAYLOAD_BYTES + 4) / 4)  // 16 words
volatile char detected_msg[11] = "NO IFR "; // updated when label arrives
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
                OSTimeDlyHMSM(0, 0, 0, 1);
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
                            start_spi    = 0;
                            video_mode   = 0;
                            bad_frames   = 0;
                            trigger_sent = 0;
                            continue;
                        }

                        if (start_spi) trigger_sent = 0;
                    }
                }
            } else if (pattern_hits == 1) {

                // Check for inference packet first
                if ((buffer[0] >> 16) == 0xC0C0 &&
                    (buffer[SPI_WORDS-1] & 0xFFFF) == 0xC0C0) {

                    // Extract label (bytes 2-7 of tx_buf → upper/lower halves of buffer[0]/buffer[1])
                    char lbl[7];
                    lbl[0] = (buffer[0] >>  8) & 0xFF;  // tx_buf[2]
                    lbl[1] = (buffer[0]      ) & 0xFF;  // tx_buf[3]
                    lbl[2] = (buffer[1] >> 24) & 0xFF;  // tx_buf[4]
                    lbl[3] = (buffer[1] >> 16) & 0xFF;  // tx_buf[5]
                    lbl[4] = (buffer[1] >>  8) & 0xFF;  // tx_buf[6]
                    lbl[5] = (buffer[1]      ) & 0xFF;  // tx_buf[7]
                    lbl[6] = '\0';

                    // Trim trailing nulls/spaces
                    for (int i = 5; i >= 0; i--) {
                        if (lbl[i] == '\0' || lbl[i] == ' ') lbl[i] = '\0';
                        else break;
                    }

                    uint8_t avg_conf = (buffer[2] >> 24) & 0xFF;  // tx_buf[8]

                    snprintf((char*)detected_msg, 11, "%-2s-%03d  ", lbl, avg_conf);

                // Then check for image data packet as before
                } else if ((buffer[0] >> 16) == 0xA0A0 &&
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

        // Always signal Core 0 with accel data
        IOWR_ALTERA_AVALON_PIO_DATA(IRQ_CORE1_TX_BASE, 0x00008000);
        IOWR_ALTERA_AVALON_PIO_DATA(IRQ_CORE1_TX_BASE, 0x00000000);

        if (z < -208) {
            IOWR_ALTERA_AVALON_PIO_DATA(ONBOARD_LEDS_BASE, emergency_led ? 0x3FF : 0x000);
            emergency_led = !emergency_led;
        }
    }
}

// --- TASK 3: UI ---
void ui_task(void* pdata) {
    int     scroll_offset = 0;
    char    cpu_message[7]= "C0-000";
    uint8_t cpu_idx       = 0;
    int     prev_sw4      = 0;
    int     prev_sw5      = 0;

    while (1) {
        OSTimeDly(1);

        char* message = (char*)detected_msg;
        int   len     = strlen(message);
        if (len == 0) len = 1;   // guard


        // Read switches every tick regardless of scroll_flag
        int sw   = IORD_ALTERA_AVALON_PIO_DATA(SWITCHES_BASE);
        int sw1  = (sw >> 0) & 0x1;
        int sw2  = (sw >> 1) & 0x1;
        int sw3  = (sw >> 2) & 0x1;
        int sw4  = (sw >> 3) & 0x1;
        int sw5  = (sw >> 4) & 0x1;
        int sw10 = (sw >> 9) & 0x1;

        if (!emergency_stop) {

            // --- ACTION SWITCHES: checked every tick ---
            if (sw4 && !prev_sw4) OSSemPost(music_sem);

            if (sw5 && !prev_sw5) {
                IOWR_ALTERA_AVALON_PIO_DATA(IRQ_CORE1_TX_BASE, 0x00007000);
                IOWR_ALTERA_AVALON_PIO_DATA(IRQ_CORE1_TX_BASE, 0x00000000);
                start_spi  = 0;
                video_mode = 0;
            }

            prev_sw4 = sw4;
            prev_sw5 = sw5;

            if (sw10) IOWR_ALTERA_AVALON_PIO_DATA(ONBOARD_LEDS_BASE, 0x0);

            // --- DISPLAY: only updated on scroll_flag ---
            if (!scroll_flag) continue;
            scroll_flag = 0;

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
        }
    }
}

// --- TASK 4: MUSIC ---
void music_task(void* pdata) {
    INT8U err;
    while (1) {
        OSSemPend(music_sem, 0, &err);
        while (OSSemAccept(music_sem) > 0);  // drain queued presses
        play_tone();
        IOWR_ALTERA_AVALON_PIO_DATA(BUZZER_BASE, NOTE_REST);
    }
}

// --- MAIN ---
#define TASK_STACKSIZE 1024
OS_STK camera_task_stk [TASK_STACKSIZE];
OS_STK accel_task_stk  [TASK_STACKSIZE];
OS_STK ui_task_stk     [TASK_STACKSIZE];
OS_STK startup_task_stk[TASK_STACKSIZE];
OS_STK music_task_stk  [TASK_STACKSIZE];

#define CAMERA_PRIORITY 1
#define ACCEL_PRIORITY  2
#define UI_PRIORITY     3
#define MUSIC_PRIORITY  4

void startup_task(void* pdata) {
    OSStatInit();
    OSTaskCreateExt(camera_spi_task, NULL, (void*)&camera_task_stk[TASK_STACKSIZE-1], CAMERA_PRIORITY, CAMERA_PRIORITY, camera_task_stk, TASK_STACKSIZE, NULL, 0);
    OSTaskCreateExt(accel_task,      NULL, (void*)&accel_task_stk [TASK_STACKSIZE-1], ACCEL_PRIORITY,  ACCEL_PRIORITY,  accel_task_stk,  TASK_STACKSIZE, NULL, 0);
    OSTaskCreateExt(ui_task,         NULL, (void*)&ui_task_stk    [TASK_STACKSIZE-1], UI_PRIORITY,     UI_PRIORITY,     ui_task_stk,     TASK_STACKSIZE, NULL, 0);
    OSTaskCreateExt(music_task,      NULL, (void*)&music_task_stk [TASK_STACKSIZE-1], MUSIC_PRIORITY,  MUSIC_PRIORITY,  music_task_stk,  TASK_STACKSIZE, NULL, 0);
    OSTaskDel(OS_PRIO_SELF);
}

int main(void) {
    alt_printf("=========================================================\n");
    alt_printf("CPU Core 1 Alive\n");
    alt_printf("=========================================================\n");

    accel = alt_up_accelerometer_spi_open_dev("/dev/accelerometer_spi_0");
    if (!accel) { alt_printf("Failed to open accelerometer\n"); return 1; }
    alt_printf("Accelerometer initialized\n");

    IOWR_ALTERA_AVALON_PIO_DATA(ONBOARD_LEDS_BASE, 0x0);

    accel_sem = OSSemCreate(0);
    music_sem = OSSemCreate(0);

    init_rx();
    init_key_pio();
    init_timer();

    OSTaskCreateExt(startup_task, NULL, (void*)&startup_task_stk[TASK_STACKSIZE-1], 0, 0, startup_task_stk, TASK_STACKSIZE, NULL, 0);
    OSStart();
    return 0;
}
