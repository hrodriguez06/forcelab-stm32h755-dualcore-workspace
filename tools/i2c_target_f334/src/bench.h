/**
  ******************************************************************************
  * @file    bench.h
  * @brief   NUCLEO-F334R8 bench firmware of 05_i2c: the I2C target at 0x42
  *          (01_shared_drivers/05_i2c/IMPLEMENTATION_PLAN.md, D1, D2, S15, S21).
  *
  *          Bus pins (UM1724 Arduino connector, plan "El bus I2C"):
  *            PB8 = I2C1_SCL, AF4, CN5-10 (D15)
  *            PB9 = I2C1_SDA, AF4, CN5-9  (D14)
  *          Console: USART2 on PA2 (TX) / PA3 (RX), AF7, the ST-LINK/V2-1 VCP,
  *          115200 8N1. The protocol is in ../README.md.
  *
  *          S15: PB8 and PB9 are only ever input without pull, AF4 open-drain
  *          without pull, or (during an announced hold) GPIO open-drain without
  *          pull. The pin_* helpers of target.c are the only writers of their
  *          GPIOB fields; target_s15_ok() checks the registers.
  *          S21: a hold drives one line low, open-drain, for 1..HOLD_MAX_MS ms;
  *          TIM2 channel 1 releases it in its interrupt at the limit whatever
  *          the main loop does, and the independent watchdog resets the chip
  *          (all pins input) if the firmware stops.
  ******************************************************************************
  */

#ifndef BENCH_H
#define BENCH_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32f3xx.h"

#define TARGET_PROTOCOL       1U          /* "I2C_TARGET v1": bump on any change of the protocol */
#define TARGET_ADDR7          0x42U
#define TARGET_PIN_SCL        8U          /* PB8 */
#define TARGET_PIN_SDA        9U          /* PB9 */
#define TARGET_AF_I2C1        4U          /* DS9994 table 14: PB8 I2C1_SCL, PB9 I2C1_SDA */
#define HOLD_MAX_MS           500U        /* S21 */
/* SCL edges seen during the first 10 us of an SDA hold are not counted (but are
   reported as blank=): in the first bench runs (2026-10-06) the F334's own fall
   of SDA, with the 150 pF tap and no series resistor, put one rising edge on
   its PB8 input 2 us later (EXTI has no filter) that no other view of SCL saw. */
#define HOLD_EDGE_BLANK_US    10U
#define STRETCH_MAX_US        5000U       /* longest armed clock stretch, per event */
#define XFER_IDLE_US          500000U     /* a transfer with no progress for this long is cut */
#define WAIT_STALL_READS      1000U       /* a timer that reads the same this often has stopped */

/* ---- main.c ------------------------------------------------------------------ */
extern uint32_t g_sysclk_hz;              /* 64 MHz, or 8 MHz if the PLL did not lock */

uint32_t us_now(void);                    /* TIM2, 1 MHz, free running                  */
bool     wait_us(uint32_t us);            /* false if TIM2 stopped counting             */

/* ---- serial.c ---------------------------------------------------------------- */
void     serial_init(uint32_t pclk1_hz);
bool     serial_getline(char *buf, uint32_t size);   /* a complete line, without CR/LF */
uint32_t serial_overruns(void);

typedef struct
{
    char     text[208];
    uint32_t n;
} Line_t;

void ln_start(Line_t *l, const char *s);
void ln_str(Line_t *l, const char *s);
void ln_u32(Line_t *l, uint32_t v);
void ln_i32(Line_t *l, int32_t v);
void ln_hex(Line_t *l, uint32_t v, uint32_t digits);
void ln_send(Line_t *l);                  /* appends CR LF and writes the line          */
void serial_puts(const char *s);          /* one whole line, CR LF appended             */

/* ---- target.c ---------------------------------------------------------------- */
void target_pins_safe(void);              /* PB8/PB9 input, open-drain type, no pull    */
bool target_s15_ok(void);
void target_init(void);
void target_poll(void);                   /* main loop: reports, end of a hold, idle cut */
void target_command(char *line);
void target_banner(uint32_t reset_flags);

#endif /* BENCH_H */
