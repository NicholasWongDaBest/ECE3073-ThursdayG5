/*
 * utility.c
 *
 *  Created on: 20 Apr 2026
 *      Author: yuen
 */

void delay(int s) {
	volatile int i;
	for (i = 0; i < s * 5000; i++);
}
