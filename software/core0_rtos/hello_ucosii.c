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

//core 0
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
OS_EVENT *rx_trigger_sem; // Semaphore to wake up the Accel task

volatile int rx_buffer;
volatile int edge_capture;
static void init_rx();
static void handle_rx_interrupts(void* context, alt_u32 id);

volatile int emergency_stop = 0; //flag for emergency stop

int* EMER_ADDR = (int*) 0x01E02580;
int* ACCE_WRADDR = (int*) 0x01E04B00;
int* CORE0_WRADDR = (int*) 0x01E04B0C;

static void init_rx() {
	void* edge_capture_ptr = (void*) &edge_capture;
	IOWR_ALTERA_AVALON_PIO_IRQ_MASK(IRQ_CORE0_RX_BASE, 0xFFFFFFFF);
	IOWR_ALTERA_AVALON_PIO_EDGE_CAP(IRQ_CORE0_RX_BASE, 0);
	alt_irq_register( IRQ_CORE0_RX_IRQ,edge_capture_ptr,handle_rx_interrupts);
}

static void handle_rx_interrupts (void* context, alt_u32 id) {
	volatile int* edge_capture_ptr = (volatile int*) context;

	int edge = IORD_ALTERA_AVALON_PIO_EDGE_CAP(IRQ_CORE0_RX_BASE);
	*edge_capture_ptr = edge;
	rx_buffer = edge;
	IOWR_ALTERA_AVALON_PIO_EDGE_CAP(IRQ_CORE0_RX_BASE, edge);
	//wakes up the Accel task
	OSSemPost(rx_trigger_sem);
}

/* Definition of Task Stacks */
#define   TASK_STACKSIZE       1024
OS_STK    accel_task_stk[TASK_STACKSIZE];
OS_STK startup_task_stk[TASK_STACKSIZE];
/* Definition of Task Priorities */
#define ACCEL_PRIORITY      1


void accel_task(void* pdata)
{
	INT8U err;
  while (1)
  { 
	  OSSemPend(rx_trigger_sem, 0, &err);
		if(rx_buffer & 0x00008000){
			//recast as signed integer
//			int32_t received_x = (int32_t) IORD_32DIRECT(ACCE_WRADDR,0);
//			int32_t received_y = (int32_t) IORD_32DIRECT(ACCE_WRADDR,4);
			int32_t received_z = (int32_t) IORD_32DIRECT(ACCE_WRADDR,8);
			rx_buffer = 0x0;
			//emergency stop
			if(received_z<-208 && !emergency_stop){
				IOWR_ALTERA_AVALON_PIO_DATA(IRQ_CORE0_TX_BASE,0x00010001);
				IOWR_ALTERA_AVALON_PIO_DATA(IRQ_CORE0_TX_BASE,0x00000000);
				emergency_stop = 1;
				alt_printf("=========================================================\n");
				alt_printf("DEVICE UPSIDE DOWN!!!\nTEMPORARY STALLING DEVICE\n");
				alt_printf("=========================================================\n");

			}
			else if(received_z>-208 && emergency_stop){
				IOWR_ALTERA_AVALON_PIO_DATA(IRQ_CORE0_TX_BASE,0x00020002);
				IOWR_ALTERA_AVALON_PIO_DATA(IRQ_CORE0_TX_BASE,0x00000000);
				alt_printf("=========================================================\n");
				alt_printf("DEVICE BACK ALIVE\n");
				alt_printf("=========================================================\n");
				emergency_stop = 0;
			}
			IOWR_32DIRECT(CORE0_WRADDR,0,OSCPUUsage);
		}
  }
}

void startup_task(void* pdata) {
    // 1. Initialize the stats (this runs while nothing else is competing for CPU)
    OSStatInit();

    // 2. Create your actual tasks NOW
    OSTaskCreateExt(accel_task,
                    NULL,
                    (void *)&accel_task_stk[TASK_STACKSIZE-1],
  				  ACCEL_PRIORITY,
  				  ACCEL_PRIORITY,
  				  accel_task_stk,
                    TASK_STACKSIZE,
                    NULL,
                    0);

    // 3. Delete this startup task, its job is done!
    OSTaskDel(OS_PRIO_SELF);
}

int main(void)
{
	alt_printf("=========================================================\n");
	alt_printf("CPU Core 0 Alive\n");
	alt_printf("=========================================================\n");


	//write emergency image into SDRAM

	rx_trigger_sem = OSSemCreate(0);

	init_rx();

	OSTaskCreateExt(startup_task, NULL, (void *)&startup_task_stk[TASK_STACKSIZE-1], 0, 0, startup_task_stk, TASK_STACKSIZE, NULL, 0);

	OSStart();

	return 0;
}

