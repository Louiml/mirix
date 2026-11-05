/* 8253/8254 PIT. Used here only to calibrate the TSC: channel 0 in
 * one-shot mode with a known divisor, then poll its count down to zero. */
#ifndef MIRIX_DRIVERS_PIT_H
#define MIRIX_DRIVERS_PIT_H

#include <stdint.h>

#define PIT_CH0_DATA 0x40
#define PIT_COMMAND  0x43
#define PIT_FREQ_HZ  1193182ULL   /* 14.31818 MHz crystal / 12 */

void pit_oneshot(uint16_t divisor);   /* mode 0, lobyte+hibyte; 0 = 65536 */
uint16_t pit_read_count(void);        /* latch channel 0 and read it */

#endif
