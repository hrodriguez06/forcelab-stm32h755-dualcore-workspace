/**
  ******************************************************************************
  * @file    target.c
  * @brief   The I2C target of the 05_i2c bench on the F334: I2C1 as a slave at
  *          0x42 on PB8/PB9, 256 registers with an auto-incrementing pointer,
  *          faults armed beforehand from the console, a report after every
  *          transfer, and bounded holds of SDA or SCL (S21). Protocol and
  *          meaning of every field: ../README.md.
  *
  *          Registers (plan, "Firmware de banco de la F334"): reg[i] = i ^ 0xA5
  *          at start and after INIT. In a write, the first data byte after the
  *          address sets the pointer and the next ones are stored from there;
  *          a read returns reg[pointer], reg[pointer + 1], ... The pointer
  *          wraps at 256 and is only advanced by bytes that went on the bus.
  *
  *          Slave mode of the I2C (RM0364 28.4.8; the same IP as RM0399 50.4.8,
  *          50:561-713): NOSTRETCH = 0. Byte control (SBC = 1, RELOAD = 1,
  *          NBYTES = 1: TCR after each byte, SCL stretched, in reception
  *          between the 8th and 9th clock, 50:632-648) is switched on at the
  *          address match ONLY when a NACK or a per-byte stretch is armed for
  *          that transfer: otherwise every byte is acknowledged with no
  *          software in the clock path.
  ******************************************************************************
  */

#include "bench.h"
#include "fw_build_id.h"

#include <stddef.h>

/* ---- Pins (S15) ------------------------------------------------------------------------ */

#define PIN_MASK2(p)   (3UL << ((p) * 2U))

static uint32_t s_hold_pin = 0xFFU;     /* the pin a hold drives, 0xFF when none */

/* The ONLY writers of the PB8/PB9 fields. OTYPER = 1 (open-drain) is written
   before MODER leaves input; PUPDR is only ever cleared; ODR = 1 (released)
   before MODER becomes output. */
static void pin_input(uint32_t pin)
{
    GPIOB->MODER &= ~PIN_MASK2(pin);
    GPIOB->PUPDR &= ~PIN_MASK2(pin);
    GPIOB->OTYPER |= 1UL << pin;
}

static void pin_af_od(uint32_t pin)
{
    GPIOB->OTYPER |= 1UL << pin;
    GPIOB->PUPDR  &= ~PIN_MASK2(pin);
    GPIOB->OSPEEDR = (GPIOB->OSPEEDR & ~PIN_MASK2(pin)) | (1UL << (pin * 2U));     /* medium */
    GPIOB->AFR[1]  = (GPIOB->AFR[1] & ~(0xFUL << ((pin - 8U) * 4U))) | (TARGET_AF_I2C1 << ((pin - 8U) * 4U));
    GPIOB->MODER   = (GPIOB->MODER & ~PIN_MASK2(pin)) | (2UL << (pin * 2U));
}

static void pin_out_od_released(uint32_t pin)
{
    GPIOB->BSRR    = 1UL << pin;                    /* ODR = 1: open-drain released */
    GPIOB->OTYPER |= 1UL << pin;
    GPIOB->PUPDR  &= ~PIN_MASK2(pin);
    GPIOB->MODER   = (GPIOB->MODER & ~PIN_MASK2(pin)) | (1UL << (pin * 2U));
}

typedef struct { uint32_t moder, otyper, pupdr, afr; } PinCfg_t;

static PinCfg_t pin_cfg(uint32_t pin)
{
    PinCfg_t c;
    c.moder  = (GPIOB->MODER >> (pin * 2U)) & 3UL;
    c.otyper = (GPIOB->OTYPER >> pin) & 1UL;
    c.pupdr  = (GPIOB->PUPDR >> (pin * 2U)) & 3UL;
    c.afr    = (GPIOB->AFR[1] >> ((pin - 8U) * 4U)) & 0xFUL;
    return c;
}

static bool pin_s15_ok(uint32_t pin)
{
    const PinCfg_t c = pin_cfg(pin);
    if (c.pupdr != 0U)
    {
        return false;                                       /* never a pull */
    }
    if (c.moder == 0U)
    {
        return true;                                        /* input */
    }
    if (c.moder == 2U)
    {
        return (c.otyper == 1U) && (c.afr == TARGET_AF_I2C1);   /* AF4 open-drain */
    }
    if (c.moder == 1U)
    {
        return (c.otyper == 1U) && (pin == s_hold_pin);     /* open-drain, in a hold only */
    }
    return false;                                           /* analog: not a bench state */
}

bool target_s15_ok(void)
{
    return pin_s15_ok(TARGET_PIN_SCL) && pin_s15_ok(TARGET_PIN_SDA);
}

void target_pins_safe(void)
{
    RCC->AHBENR |= RCC_AHBENR_GPIOBEN;
    (void)RCC->AHBENR;
    GPIOB->BSRR = (1UL << TARGET_PIN_SCL) | (1UL << TARGET_PIN_SDA);
    pin_input(TARGET_PIN_SCL);
    pin_input(TARGET_PIN_SDA);
}

static void ln_pin(Line_t *l, const char *name, uint32_t pin)
{
    const PinCfg_t c = pin_cfg(pin);
    ln_str(l, name);
    ln_str(l, " MODER=");
    ln_u32(l, c.moder);
    ln_str(l, " OTYPER=");
    ln_u32(l, c.otyper);
    ln_str(l, " PUPDR=");
    ln_u32(l, c.pupdr);
    ln_str(l, " AFR=");
    ln_u32(l, c.afr);
}

static void ln_pins(Line_t *l)
{
    ln_pin(l, " PB8", TARGET_PIN_SCL);
    ln_pin(l, " PB9", TARGET_PIN_SDA);
    ln_str(l, target_s15_ok() ? " s15=ok" : " s15=VIOLATED");
}

/* ---- I2C1 ------------------------------------------------------------------------------ */

/* PRESC = 0, SCLDEL = 10, SDADEL = 2 at a 64 MHz kernel clock (15.6 ns), by the
   rules of 50:319-353 (RM0364 28.4.5 describes the same IP):
   - data setup: SCL is held low (SCLDEL + 1) x 15.6 = 172 ns after SDA
     changes, >= t_r + tSU;DAT up to Fm+ (120 + 50 ns);
   - data output: SDA changes tSYNC1 + (SDADEL + 1) x 15.6 = tSYNC1 + 47 ns
     after SCL falls, tSYNC1 being the SCL fall, the analog filter (tAF) and
     2..3 kernel cycles. The tVD;DAT of Fm+ (450 ns) also contains the SDA
     rise, so it holds only while tSYNC1 + 47 ns + t_r <= 450 ns: a t_r of
     about 90 ns at most with the F334's largest tAF (audit F0, m7), as on the
     notaps bench (t_r ~ 50 ns), not on the taps one (225..270 ns, where the
     bench runs 400 kHz at most and tVD;DAT is 900 ns). As a receiver the
     target has no tVD;DAT to meet.
   SCLH/SCLL are not used in slave mode. These numbers need SYSCLK = 64 MHz:
   the host refuses a banner with another sysclk (the PLL did not lock and
   the kernel is the 8 MHz HSI, audit F0 m7). */
#define TARGET_TIMINGR   ((0UL << I2C_TIMINGR_PRESC_Pos) | (10UL << I2C_TIMINGR_SCLDEL_Pos) | \
                          (2UL << I2C_TIMINGR_SDADEL_Pos))

#define EV_BERR   0x01U
#define EV_ARLO   0x02U
#define EV_OVR    0x04U
#define EV_TOUT   0x08U     /* cut by XFER_IDLE_US (PE = 0) */
#define EV_HALF   0x10U     /* a write segment ended right after its address (no pointer byte) */
#define EV_LATE   0x20U     /* a TCR of reception with no byte to close (acknowledged)          */

typedef struct
{
    int32_t  nack_at;          /* -1, or index of the received data byte to NACK (0 = first) */
    int32_t  corrupt_at;       /* -1, or index of the returned byte to invert                */
    uint32_t stretch_addr_us;  /* 0, or SCL held this long at the address match             */
    uint32_t stretch_byte_us;  /* 0, or SCL held this long after each byte (byte control)    */
} Arm_t;

typedef struct
{
    uint32_t seq;
    uint32_t starts;           /* address matches: the START and every Sr                    */
    uint32_t rs;               /* repeated starts                                            */
    uint32_t stop;             /* 1: ended by a STOP                                         */
    uint32_t dirs;             /* bit k: direction of match k (1 = read), k < 16              */
    uint32_t rx;               /* data bytes received (a NACKed byte included)               */
    uint32_t tx;               /* data bytes that went on the bus                            */
    uint32_t nk;               /* NACKs given                                                */
    uint32_t nm;               /* NACKs received (the master's last read byte)               */
    uint32_t crc_rx;           /* CRC-32 (zlib) of the received bytes, in order              */
    uint32_t crc_tx;           /* CRC-32 (zlib) of the transmitted bytes, in order           */
    uint32_t p0, p1;           /* pointer at the start and at the end                        */
    uint32_t ev;               /* EV_*                                                       */
    uint32_t us;               /* first address match to the end, TIM2 microseconds          */
    uint32_t t0;
    Arm_t    arm;
} Xfer_t;

#define RPT_RING   8U

static uint8_t           s_reg[256];
static volatile uint32_t s_ptr;

static const Arm_t       k_disarmed = { -1, -1, 0U, 0U };
static Arm_t             s_arm;                 /* for the next transfer; written with IRQs masked */

static Xfer_t            s_x;                   /* the transfer in progress, owned by the ISR */
static volatile bool     s_active;
static volatile uint32_t s_last_us;
static bool              s_seg_first;           /* the next byte received is the pointer       */
static bool              s_rx_nack_this;
static bool              s_tx_pending;          /* a byte written to TXDR, not yet shifted out */
static uint8_t           s_tx_pending_byte;
static uint32_t          s_seq;

static Xfer_t            s_rpt[RPT_RING];
static volatile uint32_t s_rpt_head;
static volatile uint32_t s_rpt_tail;
static volatile uint32_t s_rpt_dropped;

static volatile uint32_t s_tot_xfers, s_tot_rx, s_tot_tx, s_tot_nk, s_tot_ev, s_idle_errs;
static uint32_t          s_fmp;
static bool              s_aborted;             /* S15 violation seen: bus pins input, I2C off */

static uint32_t crc_byte(uint32_t state, uint8_t b)
{
    state ^= b;
    for (uint32_t k = 0U; k < 8U; k++)
    {
        state = ((state & 1U) != 0U) ? ((state >> 1) ^ 0xEDB88320UL) : (state >> 1);
    }
    return state;
}

/* PE = 0 releases SCL and SDA and resets the state machine; PE must stay 0 for
   3 APB cycles (RM0364 28.4.6), read back. */
static void i2c_pe(bool on)
{
    if (on)
    {
        I2C1->CR1 |= I2C_CR1_PE;
    }
    else
    {
        I2C1->CR1 &= ~I2C_CR1_PE;
        for (uint32_t i = 0U; (i < 3U) || ((I2C1->CR1 & I2C_CR1_PE) != 0U); i++)
        {
            if (i > 100U)
            {
                break;
            }
        }
    }
}

static void i2c_init_regs(void)
{
    RCC->CFGR3   |= RCC_CFGR3_I2C1SW;                   /* I2C1 kernel = SYSCLK */
    RCC->APB1ENR |= RCC_APB1ENR_I2C1EN;
    (void)RCC->APB1ENR;
    i2c_pe(false);
    I2C1->CR2     = 0U;
    I2C1->TIMINGR = TARGET_TIMINGR;
    I2C1->OAR1    = 0U;
    I2C1->OAR2    = 0U;
    I2C1->OAR1    = I2C_OAR1_OA1EN | (TARGET_ADDR7 << 1);
    I2C1->ICR     = I2C_ICR_ADDRCF | I2C_ICR_NACKCF | I2C_ICR_STOPCF | I2C_ICR_BERRCF |
                    I2C_ICR_ARLOCF | I2C_ICR_OVRCF;
    I2C1->CR1     = I2C_CR1_ADDRIE | I2C_CR1_RXIE | I2C_CR1_TXIE | I2C_CR1_STOPIE |
                    I2C_CR1_NACKIE | I2C_CR1_ERRIE | I2C_CR1_TCIE;
}

void target_init(void)
{
    for (uint32_t i = 0U; i < 256U; i++)
    {
        s_reg[i] = (uint8_t)(i ^ 0xA5U);
    }
    s_ptr = 0U;
    s_arm = k_disarmed;

    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    (void)RCC->APB2ENR;
    /* EXTI line 8 from PB8 (SCL), once: the SDA holds count SCL rising edges
       on it. Masked and with no trigger edge until a hold arms it. */
    EXTI->IMR  &= ~EXTI_IMR_MR8;
    EXTI->RTSR &= ~EXTI_RTSR_TR8;
    EXTI->FTSR &= ~EXTI_FTSR_TR8;
    SYSCFG->EXTICR[2] = (SYSCFG->EXTICR[2] & ~SYSCFG_EXTICR3_EXTI8) | SYSCFG_EXTICR3_EXTI8_PB;
    EXTI->PR = EXTI_PR_PR8;
    target_pins_safe();
    i2c_init_regs();                       /* PE = 0: the pins can go to AF, nothing drives */
    pin_af_od(TARGET_PIN_SCL);
    pin_af_od(TARGET_PIN_SDA);
    NVIC_SetPriority(I2C1_EV_IRQn, 1U);   /* below TIM2, the end of a hold (main.c, m3) */
    NVIC_SetPriority(I2C1_ER_IRQn, 1U);
    NVIC_EnableIRQ(I2C1_EV_IRQn);
    NVIC_EnableIRQ(I2C1_ER_IRQn);
    if (target_s15_ok())
    {
        i2c_pe(true);
    }
}

/* ---- The transfer, in the interrupt ---------------------------------------------------- */

static void rpt_push(const Xfer_t *x)
{
    const uint32_t next = (s_rpt_head + 1U) % RPT_RING;
    if (next == s_rpt_tail)
    {
        s_rpt_dropped++;
        return;
    }
    s_rpt[s_rpt_head] = *x;
    s_rpt_head = next;
}

static void tx_commit(void)
{
    s_x.tx++;
    s_x.crc_tx = crc_byte(s_x.crc_tx, s_tx_pending_byte);
    s_tx_pending = false;
}

/* End of a read segment: a byte still in TXDR (TXE = 0) never reached the
   shift register, so it was not sent; the pointer goes back by one. */
static void tx_settle(void)
{
    if (s_tx_pending)
    {
        if ((I2C1->ISR & I2C_ISR_TXE) != 0U)
        {
            tx_commit();
        }
        else
        {
            s_ptr = (s_ptr - 1U) & 0xFFU;
            s_tx_pending = false;
        }
    }
    I2C1->ISR = I2C_ISR_TXE;                    /* flush (RM0364 28.4.8, slave transmitter) */
}

static void xfer_begin(uint32_t now)
{
    s_x.seq = ++s_seq;
    s_x.starts = 0U;
    s_x.rs = 0U;
    s_x.stop = 0U;
    s_x.dirs = 0U;
    s_x.rx = 0U;
    s_x.tx = 0U;
    s_x.nk = 0U;
    s_x.nm = 0U;
    s_x.crc_rx = 0xFFFFFFFFUL;
    s_x.crc_tx = 0xFFFFFFFFUL;
    s_x.p0 = s_ptr;
    s_x.p1 = s_ptr;
    s_x.ev = 0U;
    s_x.us = 0U;
    s_x.t0 = now;
    s_x.arm = s_arm;                             /* one-shot: the arming is consumed here */
    s_arm = k_disarmed;
    s_tx_pending = false;
    s_rx_nack_this = false;
    s_active = true;
}

static void xfer_end(uint32_t ev)
{
    tx_settle();
    if (s_seg_first)
    {
        s_x.ev |= EV_HALF;
    }
    s_x.ev    |= ev;
    s_x.p1     = s_ptr;
    s_x.us     = us_now() - s_x.t0;
    s_x.crc_rx = ~s_x.crc_rx;
    s_x.crc_tx = ~s_x.crc_tx;
    s_tot_xfers++;
    s_tot_rx += s_x.rx;
    s_tot_tx += s_x.tx;
    s_tot_nk += s_x.nk;
    if (s_x.ev != 0U)
    {
        s_tot_ev++;
    }
    rpt_push(&s_x);
    s_active = false;
    s_seg_first = false;
    I2C1->CR1 &= ~I2C_CR1_SBC;                   /* SBC changes only while not addressed */
    I2C1->CR2  = 0U;
}

/* One received byte: counted, folded into the CRC and, unless it is the byte
   armed to be NACKed, stored (the first of a write segment sets the pointer).
   Whether to NACK it is decided here and applied at its TCR (byte control). */
static void rx_take(uint32_t now)
{
    const uint8_t b = (uint8_t)I2C1->RXDR;
    if (!s_active)
    {
        return;
    }
    const uint32_t idx = s_x.rx;
    s_x.rx++;
    s_x.crc_rx = crc_byte(s_x.crc_rx, b);
    s_rx_nack_this = (s_x.arm.nack_at >= 0) && (idx == (uint32_t)s_x.arm.nack_at);
    if (!s_rx_nack_this)
    {
        if (s_seg_first)
        {
            s_ptr = b;
            s_seg_first = false;
        }
        else
        {
            s_reg[s_ptr] = b;
            s_ptr = (s_ptr + 1U) & 0xFFU;
        }
    }
    s_last_us = now;
}

/* A TCR of reception read before the RXNE of its own byte (audit F0, M1): the
   byte is waited for, a few bus bits at most, and taken first, so the ACK or
   NACK written at this TCR is the one of this byte. Bounded by TIM2 and by a
   count of ISR reads (TIM2 may be the thing that stopped). */
#define TCR_RXNE_WAIT_US     200U
#define TCR_RXNE_WAIT_READS  20000U

static bool rx_wait_take(uint32_t now)
{
    for (uint32_t n = 0U; n < TCR_RXNE_WAIT_READS; n++)
    {
        if ((I2C1->ISR & I2C_ISR_RXNE) != 0U)
        {
            rx_take(now);
            return true;
        }
        if ((us_now() - now) > TCR_RXNE_WAIT_US)
        {
            break;
        }
    }
    return false;
}

/* The flags of one ISR read, in this order (audit F0, M1 and m4): errors; RXNE
   before TCR (the TCR of reception closes the byte RXNE delivers); TXIS; NACKF
   and STOPF, which close the transfer, before ADDR, which opens the next one.
   A flag that appears while this runs is served by the next interrupt. */
static void i2c_irq(void)
{
    const uint32_t isr = I2C1->ISR;
    const uint32_t now = us_now();

    const uint32_t errs = isr & (I2C_ISR_BERR | I2C_ISR_ARLO | I2C_ISR_OVR);
    if (errs != 0U)
    {
        I2C1->ICR = errs;                        /* the ICR bits sit at the ISR positions */
        if (s_active)
        {
            s_x.ev |= (((errs & I2C_ISR_BERR) != 0U) ? EV_BERR : 0U) |
                      (((errs & I2C_ISR_ARLO) != 0U) ? EV_ARLO : 0U) |
                      (((errs & I2C_ISR_OVR) != 0U) ? EV_OVR : 0U);
        }
        else
        {
            s_tot_ev++;
        }
    }

    if ((isr & I2C_ISR_RXNE) != 0U)
    {
        rx_take(now);
    }

    if ((isr & I2C_ISR_TCR) != 0U)
    {
        /* Byte control: SCL is stretched until NBYTES is written again; in
           reception the ACK bit of this byte is sent then (50:632-643). */
        bool late = false;
        if (((isr & I2C_ISR_DIR) == 0U) && ((isr & I2C_ISR_RXNE) == 0U))
        {
            late = !rx_wait_take(now);           /* M1: this TCR's byte first */
        }
        if (s_x.arm.stretch_byte_us != 0U)
        {
            (void)wait_us(s_x.arm.stretch_byte_us);
        }
        uint32_t cr2 = I2C_CR2_RELOAD | (1UL << I2C_CR2_NBYTES_Pos);
        if (s_rx_nack_this && !late)
        {
            cr2 |= I2C_CR2_NACK;
            s_x.nk++;
        }
        if (late && s_active)
        {
            s_x.ev |= EV_LATE;                   /* no byte for this TCR: ACKed, recorded */
        }
        s_rx_nack_this = false;
        I2C1->CR2 = cr2;
        s_last_us = now;
    }

    if ((isr & I2C_ISR_TXIS) != 0U)
    {
        if (s_active)
        {
            if (s_tx_pending)
            {
                tx_commit();                     /* TXDR empty: the pending byte was shifted out */
            }
            /* CORRUPT k: the k-th byte of this transfer that goes on the bus
               (audit F0, m5). A byte flushed before it went out did not count,
               so its replacement carries the same index. */
            uint8_t b = s_reg[s_ptr];
            if ((s_x.arm.corrupt_at >= 0) && (s_x.tx == (uint32_t)s_x.arm.corrupt_at))
            {
                b = (uint8_t)~b;
            }
            I2C1->TXDR = b;
            s_tx_pending = true;
            s_tx_pending_byte = b;
            s_ptr = (s_ptr + 1U) & 0xFFU;
            s_last_us = now;
        }
        else
        {
            I2C1->TXDR = 0xFFU;
        }
    }

    if ((isr & I2C_ISR_NACKF) != 0U)
    {
        I2C1->ICR = I2C_ICR_NACKCF;
        if (s_active)
        {
            s_x.nm++;
            tx_settle();
        }
    }

    if ((isr & I2C_ISR_STOPF) != 0U)
    {
        I2C1->ICR = I2C_ICR_STOPCF;
        if (s_active)
        {
            s_x.stop = 1U;
            xfer_end(0U);
        }
    }

    if ((isr & I2C_ISR_ADDR) != 0U)
    {
        const bool rd = (isr & I2C_ISR_DIR) != 0U;
        if (!s_active)
        {
            xfer_begin(now);
        }
        else
        {
            tx_settle();                         /* a repeated start ends the previous segment */
            s_x.rs++;
        }
        if (s_x.starts < 16U)
        {
            s_x.dirs |= (rd ? 1UL : 0UL) << s_x.starts;
        }
        s_x.starts++;
        s_seg_first = !rd;
        s_rx_nack_this = false;
        const bool sbc = (s_x.arm.stretch_byte_us != 0U) || (!rd && (s_x.arm.nack_at >= 0));
        if (sbc)
        {
            I2C1->CR1 |= I2C_CR1_SBC;            /* allowed with ADDR = 1 (50:646-648) */
            I2C1->CR2  = I2C_CR2_RELOAD | (1UL << I2C_CR2_NBYTES_Pos);
        }
        else
        {
            I2C1->CR1 &= ~I2C_CR1_SBC;
            I2C1->CR2  = 0U;
        }
        if (rd)
        {
            I2C1->ISR = I2C_ISR_TXE;             /* flush a stale TXDR before the first byte */
        }
        if (s_x.arm.stretch_addr_us != 0U)
        {
            (void)wait_us(s_x.arm.stretch_addr_us);   /* SCL is held low while ADDR = 1 */
        }
        I2C1->ICR = I2C_ICR_ADDRCF;
        s_last_us = now;
    }
}

void I2C1_EV_IRQHandler(void)
{
    i2c_irq();
}

void I2C1_ER_IRQHandler(void)
{
    i2c_irq();
}

/* ---- Holds (S21) ----------------------------------------------------------------------- */

#define CAUSE_TIME    1U
#define CAUSE_EDGES   2U
#define CAUSE_ABORT   3U

typedef struct
{
    uint32_t          pin;
    uint32_t          ms;
    uint32_t          edges_wanted;     /* 0: release on time only */
    volatile uint32_t edges;            /* SCL rising edges counted while SDA is held */
    volatile uint32_t blank;            /* ... and seen in the first HOLD_EDGE_BLANK_US */
    volatile uint32_t edge1;            /* TIM2 time of the first counted one          */
    volatile uint32_t t0, t1;
    volatile uint32_t cause;
    volatile bool     on;
    volatile bool     done;
} Hold_t;

static Hold_t s_hold;

/* Releases the held line. Interrupt context (TIM2 CC1, EXTI8) or masked. */
static void hold_release(uint32_t cause)
{
    if (!s_hold.on)
    {
        return;
    }
    GPIOB->BSRR = 1UL << s_hold.pin;              /* ODR = 1: released first */
    s_hold.t1 = us_now();
    pin_input(s_hold.pin);
    TIM2->DIER &= ~TIM_DIER_CC1IE;
    EXTI->IMR  &= ~EXTI_IMR_MR8;
    EXTI->RTSR &= ~EXTI_RTSR_TR8;
    EXTI->PR    = EXTI_PR_PR8;
    s_hold.cause = cause;
    s_hold.on = false;
    s_hold_pin = 0xFFU;
    s_hold.done = true;
}

void TIM2_IRQHandler(void)
{
    if ((TIM2->SR & TIM_SR_CC1IF) != 0U)
    {
        TIM2->SR = ~TIM_SR_CC1IF;
        hold_release(CAUSE_TIME);
    }
}

void EXTI9_5_IRQHandler(void)
{
    if ((EXTI->PR & EXTI_PR_PR8) != 0U)
    {
        EXTI->PR = EXTI_PR_PR8;
        if (s_hold.on)
        {
            const uint32_t now = us_now();
            if ((now - s_hold.t0) < HOLD_EDGE_BLANK_US)
            {
                s_hold.blank++;                 /* the glitch of SDA's own fall: not a clock */
                return;
            }
            if (s_hold.edges == 0U)
            {
                s_hold.edge1 = now;
            }
            s_hold.edges++;
            if ((s_hold.edges_wanted != 0U) && (s_hold.edges >= s_hold.edges_wanted))
            {
                hold_release(CAUSE_EDGES);
            }
        }
    }
}

static const char *hold_start(uint32_t pin, uint32_t ms, uint32_t edges)
{
    if ((ms == 0U) || (ms > HOLD_MAX_MS))
    {
        return "ms must be 1..500 (S21)";
    }
    if ((edges != 0U) && (pin != TARGET_PIN_SDA))
    {
        return "EDGES only with SDA (they are counted on SCL)";
    }
    if (edges > 255U)
    {
        return "edges must be 1..255";
    }
    if (s_active || s_hold.on || s_hold.done)
    {
        return "busy";
    }
    i2c_pe(false);                                /* the I2C lets go of both lines */
    pin_input(TARGET_PIN_SCL);
    pin_input(TARGET_PIN_SDA);
    s_hold_pin = pin;
    pin_out_od_released(pin);
    if (!target_s15_ok())
    {
        s_hold_pin = 0xFFU;
        target_pins_safe();
        s_aborted = true;
        return "S15 check failed, pins input";
    }
    s_hold.pin = pin;
    s_hold.ms = ms;
    s_hold.edges_wanted = edges;
    s_hold.edges = 0U;
    s_hold.blank = 0U;
    s_hold.done = false;

    __disable_irq();
    if (pin == TARGET_PIN_SDA)
    {
        /* EXTI8 has been on PB8 since target_init(): switching the mux here
           (PA8 -> PB8) can itself latch a rising edge, which the first
           version of this firmware counted (edges=1 in the first bench run).
           The edge detector is armed, then its pending bit cleared, then the
           interrupt unmasked. */
        EXTI->RTSR |= EXTI_RTSR_TR8;
        (void)EXTI->RTSR;
        __DSB();
        EXTI->PR    = EXTI_PR_PR8;
        EXTI->IMR  |= EXTI_IMR_MR8;
    }
    s_hold.t0   = us_now();
    TIM2->CCR1  = s_hold.t0 + (ms * 1000U);
    TIM2->SR    = ~TIM_SR_CC1IF;
    TIM2->DIER |= TIM_DIER_CC1IE;
    s_hold.on   = true;
    GPIOB->BSRR = 1UL << (pin + 16U);             /* ODR = 0: the open-drain pulls low */
    __enable_irq();
    return NULL;
}

/* After the release: 1 ms, both levels read, the pins back to AF4, PE = 1. */
static void hold_finish(void)
{
    (void)wait_us(1000U);
    const uint32_t idr = GPIOB->IDR;
    Line_t l;
    ln_start(&l, "HOLD_END line=");
    ln_str(&l, (s_hold.pin == TARGET_PIN_SDA) ? "SDA" : "SCL");
    ln_str(&l, " ms=");
    ln_u32(&l, s_hold.ms);
    ln_str(&l, " us=");
    ln_u32(&l, s_hold.t1 - s_hold.t0);
    ln_str(&l, " edges=");
    ln_u32(&l, s_hold.edges);
    ln_str(&l, " blank=");
    ln_u32(&l, s_hold.blank);
    ln_str(&l, " edge1_us=");
    if (s_hold.edges != 0U)
    {
        ln_u32(&l, s_hold.edge1 - s_hold.t0);       /* after the line went low */
    }
    else
    {
        ln_str(&l, "-");
    }
    ln_str(&l, " cause=");
    ln_str(&l, (s_hold.cause == CAUSE_TIME) ? "TIME" : ((s_hold.cause == CAUSE_EDGES) ? "EDGES" : "ABORT"));
    ln_str(&l, " scl=");
    ln_u32(&l, (idr >> TARGET_PIN_SCL) & 1UL);
    ln_str(&l, " sda=");
    ln_u32(&l, (idr >> TARGET_PIN_SDA) & 1UL);
    if (!s_aborted)
    {
        pin_af_od(TARGET_PIN_SCL);
        pin_af_od(TARGET_PIN_SDA);
        if (target_s15_ok())
        {
            i2c_pe(true);
        }
    }
    ln_pins(&l);
    s_hold.done = false;
    ln_send(&l);
}

/* ---- Main loop side -------------------------------------------------------------------- */

static void print_report(const Xfer_t *x)
{
    static const char k_dir[2] = { 'W', 'R' };
    char dirs[18];
    uint32_t n = (x->starts < 16U) ? x->starts : 16U;
    for (uint32_t k = 0U; k < n; k++)
    {
        dirs[k] = k_dir[(x->dirs >> k) & 1U];
    }
    dirs[n] = (x->starts > 16U) ? '+' : '\0';
    dirs[n + 1U] = '\0';
    Line_t l;
    ln_start(&l, "RPT n=");
    ln_u32(&l, x->seq);
    ln_str(&l, " st=");
    ln_u32(&l, x->starts);
    ln_str(&l, " sr=");
    ln_u32(&l, x->rs);
    ln_str(&l, " sp=");
    ln_u32(&l, x->stop);
    ln_str(&l, " dir=");
    ln_str(&l, dirs);
    ln_str(&l, " rx=");
    ln_u32(&l, x->rx);
    ln_str(&l, " tx=");
    ln_u32(&l, x->tx);
    ln_str(&l, " nk=");
    ln_u32(&l, x->nk);
    ln_str(&l, " nm=");
    ln_u32(&l, x->nm);
    ln_str(&l, " crc_rx=");
    ln_hex(&l, x->crc_rx, 8U);
    ln_str(&l, " crc_tx=");
    ln_hex(&l, x->crc_tx, 8U);
    ln_str(&l, " p0=");
    ln_u32(&l, x->p0);
    ln_str(&l, " p1=");
    ln_u32(&l, x->p1);
    ln_str(&l, " ev=");
    ln_hex(&l, x->ev, 2U);
    ln_str(&l, " us=");
    ln_u32(&l, x->us);
    ln_send(&l);
}

void target_poll(void)
{
    while (s_rpt_tail != s_rpt_head)
    {
        print_report(&s_rpt[s_rpt_tail]);
        s_rpt_tail = (s_rpt_tail + 1U) % RPT_RING;
    }
    if (s_hold.done)
    {
        hold_finish();
    }
    /* A transfer that stopped making progress (a master gone mid-byte while
       this target drives SDA, say) is cut: PE = 0 releases both lines. The time
       since the last progress is read with interrupts masked: read in two steps,
       an I2C interrupt between them made s_last_us newer than the time read
       first, the difference wrapped, and live transfers were cut after a few
       bytes (05_i2c F1 development runs, docs/i2c_dev_f1_bench_notaps_*: RPT
       ev=08 with us of a few hundred). */
    __disable_irq();
    const uint32_t idle_us = us_now() - s_last_us;
    const bool cut = s_active && (idle_us > XFER_IDLE_US) && (idle_us < 0x80000000UL);
    __enable_irq();
    if (cut)
    {
        __disable_irq();
        if (s_active)
        {
            i2c_pe(false);
            s_idle_errs++;
            xfer_end(EV_TOUT);
            if (target_s15_ok())
            {
                i2c_pe(true);
            }
        }
        __enable_irq();
    }
    /* The S15 check reads MODER and then s_hold_pin; a hold released by its
       interrupt (TIM2, or EXTI8 at the n-th SCL edge) between the two reads
       looked like an output pin with no hold, and aborted (05_i2c F1
       development run, docs/i2c_dev_f1_bench_notaps_*: the first hold released
       by EDGES). The check runs with interrupts masked, so it sees one state. */
    __disable_irq();
    const bool s15 = s_aborted || target_s15_ok();
    __enable_irq();
    if (!s15)
    {
        __disable_irq();
        hold_release(CAUSE_ABORT);
        i2c_pe(false);
        target_pins_safe();
        s_aborted = true;
        __enable_irq();
        Line_t l;
        ln_start(&l, "I2C_TARGET ABORT S15: bus pins back to input, I2C1 off;");
        ln_pins(&l);
        ln_send(&l);
    }
}

void target_banner(uint32_t reset_flags)
{
    Line_t l;
    ln_start(&l, "I2C_TARGET v");
    ln_u32(&l, TARGET_PROTOCOL);
    ln_str(&l, " Rev " FW_GIT_REV " id " FW_BUILD_ID);
    ln_send(&l);

    ln_start(&l, "I2C_TARGET pins");
    ln_pins(&l);
    ln_send(&l);

    const uint32_t *uid = (const uint32_t *)UID_BASE;
    ln_start(&l, "I2C_TARGET addr=0x");
    ln_hex(&l, TARGET_ADDR7, 2U);
    ln_str(&l, " sysclk=");
    ln_u32(&l, g_sysclk_hz);
    ln_str(&l, " timingr=0x");
    ln_hex(&l, I2C1->TIMINGR, 8U);
    ln_str(&l, " idcode=0x");
    ln_hex(&l, DBGMCU->IDCODE, 8U);
    ln_str(&l, " uid=");
    ln_hex(&l, uid[2], 8U);
    ln_hex(&l, uid[1], 8U);
    ln_hex(&l, uid[0], 8U);
    ln_str(&l, " reset_csr=0x");
    ln_hex(&l, reset_flags, 8U);
    ln_str(&l, " hold_max_ms=");
    ln_u32(&l, HOLD_MAX_MS);
    ln_send(&l);

    serial_puts(s_aborted ? "I2C_TARGET aborted (S15)" : "I2C_TARGET ready");
}

/* ---- Commands -------------------------------------------------------------------------- */

static bool parse_u32(const char *s, uint32_t *out)
{
    uint32_t v = 0U;
    uint32_t base = 10U;
    if ((s == NULL) || (*s == '\0'))
    {
        return false;
    }
    if ((s[0] == '0') && ((s[1] == 'x') || (s[1] == 'X')))
    {
        base = 16U;
        s += 2;
        if (*s == '\0')
        {
            return false;
        }
    }
    for (; *s != '\0'; s++)
    {
        uint32_t d;
        if ((*s >= '0') && (*s <= '9'))
        {
            d = (uint32_t)(*s - '0');
        }
        else if ((base == 16U) && (*s >= 'A') && (*s <= 'F'))
        {
            d = (uint32_t)(*s - 'A') + 10U;
        }
        else if ((base == 16U) && (*s >= 'a') && (*s <= 'f'))
        {
            d = (uint32_t)(*s - 'a') + 10U;
        }
        else
        {
            return false;
        }
        if (v > ((0xFFFFFFFFUL - d) / base))
        {
            return false;
        }
        v = (v * base) + d;
    }
    *out = v;
    return true;
}

static bool same(const char *a, const char *b)
{
    if ((a == NULL) || (b == NULL))
    {
        return false;
    }
    for (; (*a != '\0') && (*b != '\0'); a++, b++)
    {
        char ca = *a;
        if ((ca >= 'a') && (ca <= 'z'))
        {
            ca = (char)(ca - 'a' + 'A');
        }
        if (ca != *b)
        {
            return false;
        }
    }
    return (*a == '\0') && (*b == '\0');
}

static void reply_err(const char *cmd, const char *why)
{
    Line_t l;
    ln_start(&l, "ERR ");
    ln_str(&l, cmd);
    ln_str(&l, " ");
    ln_str(&l, why);
    ln_send(&l);
}

static void ln_arm(Line_t *l, const Arm_t *a)
{
    ln_str(l, " nack=");
    ln_i32(l, a->nack_at);
    ln_str(l, " corrupt=");
    ln_i32(l, a->corrupt_at);
    ln_str(l, " stretch_addr_us=");
    ln_u32(l, a->stretch_addr_us);
    ln_str(l, " stretch_byte_us=");
    ln_u32(l, a->stretch_byte_us);
}

static void reply_armed(const char *cmd)
{
    Arm_t a;
    __disable_irq();
    a = s_arm;
    __enable_irq();
    Line_t l;
    ln_start(&l, "OK ");
    ln_str(&l, cmd);
    ln_arm(&l, &a);
    ln_send(&l);
}

static void cmd_levels(void)
{
    const uint32_t idr = GPIOB->IDR;
    Line_t l;
    ln_start(&l, "LVL scl=");
    ln_u32(&l, (idr >> TARGET_PIN_SCL) & 1UL);
    ln_str(&l, " sda=");
    ln_u32(&l, (idr >> TARGET_PIN_SDA) & 1UL);
    ln_str(&l, " busy=");
    ln_u32(&l, ((I2C1->ISR & I2C_ISR_BUSY) != 0U) ? 1U : 0U);
    ln_str(&l, " pe=");
    ln_u32(&l, ((I2C1->CR1 & I2C_CR1_PE) != 0U) ? 1U : 0U);
    ln_str(&l, " fmp=");
    ln_u32(&l, s_fmp);
    ln_pins(&l);
    ln_send(&l);
}

static void cmd_status(void)
{
    Arm_t a;
    __disable_irq();
    a = s_arm;
    __enable_irq();
    Line_t l;
    ln_start(&l, "STATUS xfers=");
    ln_u32(&l, s_tot_xfers);
    ln_str(&l, " rx=");
    ln_u32(&l, s_tot_rx);
    ln_str(&l, " tx=");
    ln_u32(&l, s_tot_tx);
    ln_str(&l, " nk=");
    ln_u32(&l, s_tot_nk);
    ln_str(&l, " ev=");
    ln_u32(&l, s_tot_ev);
    ln_str(&l, " idle_cuts=");
    ln_u32(&l, s_idle_errs);
    ln_str(&l, " dropped=");
    ln_u32(&l, s_rpt_dropped);
    ln_str(&l, " uart_ovr=");
    ln_u32(&l, serial_overruns());
    ln_str(&l, " ptr=");
    ln_u32(&l, s_ptr);
    ln_str(&l, " active=");
    ln_u32(&l, s_active ? 1U : 0U);
    ln_str(&l, " hold=");
    ln_u32(&l, s_hold.on ? 1U : 0U);
    ln_str(&l, " aborted=");
    ln_u32(&l, s_aborted ? 1U : 0U);
    ln_arm(&l, &a);
    ln_send(&l);
}

static void cmd_dump(const char *sa, const char *sn)
{
    uint32_t a;
    uint32_t n;
    if (!parse_u32(sa, &a) || !parse_u32(sn, &n) || (a > 255U) || (n == 0U) || (n > 32U))
    {
        reply_err("DUMP", "usage: DUMP <addr 0..255> <n 1..32>");
        return;
    }
    Line_t l;
    ln_start(&l, "OK DUMP a=");
    ln_u32(&l, a);
    ln_str(&l, " n=");
    ln_u32(&l, n);
    ln_str(&l, " data=");
    for (uint32_t i = 0U; i < n; i++)
    {
        ln_hex(&l, s_reg[(a + i) & 0xFFU], 2U);
    }
    ln_send(&l);
}

/* Tokens of a command line; a line with more than MAX_TOK is refused whole,
   and every command checks its own count (audit F0, m6: a token nobody reads
   must not pass silently). */
#define MAX_TOK   5U

void target_command(char *line)
{
    const char *tok[MAX_TOK] = { NULL, NULL, NULL, NULL, NULL };
    uint32_t ntok = 0U;
    for (char *p = line; *p != '\0';)
    {
        while (*p == ' ')
        {
            *p++ = '\0';
        }
        if (*p == '\0')
        {
            break;
        }
        if (ntok < MAX_TOK)
        {
            tok[ntok] = p;
        }
        ntok++;
        while ((*p != '\0') && (*p != ' '))
        {
            p++;
        }
    }
    if (ntok == 0U)
    {
        return;
    }
    const char *c = tok[0];
    if (ntok > MAX_TOK)
    {
        reply_err(c, "too many arguments");
        return;
    }
    if ((same(c, "ID") || same(c, "LEVELS") || same(c, "STATUS") || same(c, "CLR") || same(c, "INIT") ||
         same(c, "DISARM")) && (ntok != 1U))
    {
        reply_err(c, "takes no argument");
        return;
    }

    if (same(c, "ID"))
    {
        target_banner(0U);
        return;
    }
    if (same(c, "LEVELS"))
    {
        cmd_levels();
        return;
    }
    if (same(c, "STATUS"))
    {
        cmd_status();
        return;
    }
    if (same(c, "DUMP"))
    {
        if (ntok != 3U)
        {
            reply_err(c, "usage: DUMP <addr 0..255> <n 1..32>");
            return;
        }
        cmd_dump(tok[1], tok[2]);
        return;
    }
    if (s_aborted)
    {
        reply_err(c, "aborted (S15): only ID, LEVELS, STATUS, DUMP");
        return;
    }
    if (same(c, "CLR"))
    {
        __disable_irq();
        s_tot_xfers = s_tot_rx = s_tot_tx = s_tot_nk = s_tot_ev = s_idle_errs = 0U;
        s_rpt_dropped = 0U;
        s_seq = 0U;
        __enable_irq();
        serial_puts("OK CLR");
        return;
    }
    if (same(c, "INIT"))
    {
        if (s_active)
        {
            reply_err(c, "busy");
            return;
        }
        for (uint32_t i = 0U; i < 256U; i++)
        {
            s_reg[i] = (uint8_t)(i ^ 0xA5U);
        }
        s_ptr = 0U;
        serial_puts("OK INIT reg[i]=i^0xA5 ptr=0");
        return;
    }
    if (same(c, "DISARM"))
    {
        __disable_irq();
        s_arm = k_disarmed;
        __enable_irq();
        reply_armed("DISARM");
        return;
    }
    if (same(c, "NACK") || same(c, "CORRUPT"))
    {
        uint32_t k;
        if ((ntok != 2U) || !parse_u32(tok[1], &k) || (k > 65535U))
        {
            reply_err(c, "usage: NACK|CORRUPT <byte index 0..65535>");
            return;
        }
        __disable_irq();
        if (same(c, "NACK"))
        {
            s_arm.nack_at = (int32_t)k;
        }
        else
        {
            s_arm.corrupt_at = (int32_t)k;
        }
        __enable_irq();
        reply_armed(same(c, "NACK") ? "NACK" : "CORRUPT");
        return;
    }
    if (same(c, "STRETCH"))
    {
        uint32_t us;
        const bool addr = same(tok[1], "ADDR");
        const bool byte = same(tok[1], "BYTE");
        if ((ntok != 3U) || (!addr && !byte) || !parse_u32(tok[2], &us) || (us > STRETCH_MAX_US))
        {
            reply_err(c, "usage: STRETCH ADDR|BYTE <us 0..5000>");
            return;
        }
        __disable_irq();
        if (addr)
        {
            s_arm.stretch_addr_us = us;
        }
        else
        {
            s_arm.stretch_byte_us = us;
        }
        __enable_irq();
        reply_armed("STRETCH");
        return;
    }
    if (same(c, "FMP"))
    {
        uint32_t on;
        if ((ntok != 2U) || !parse_u32(tok[1], &on) || (on > 1U))
        {
            reply_err(c, "usage: FMP 0|1");
            return;
        }
        const uint32_t bits = SYSCFG_CFGR1_I2C_PB8_FMP | SYSCFG_CFGR1_I2C_PB9_FMP;
        __disable_irq();
        SYSCFG->CFGR1 = (on != 0U) ? (SYSCFG->CFGR1 | bits) : (SYSCFG->CFGR1 & ~bits);
        __enable_irq();
        const uint32_t got = SYSCFG->CFGR1 & bits;
        s_fmp = (got == bits) ? 1U : 0U;
        Line_t l;
        ln_start(&l, ((got == bits) == (on != 0U)) ? "OK FMP " : "ERR FMP read back ");
        ln_u32(&l, s_fmp);
        ln_str(&l, " cfgr1=0x");
        ln_hex(&l, SYSCFG->CFGR1, 8U);
        ln_send(&l);
        return;
    }
    if (same(c, "HOLD"))
    {
        /* HOLD SDA|SCL <ms 1..500> [EDGES <n>] */
        uint32_t ms;
        uint32_t edges = 0U;
        const bool sda = same(tok[1], "SDA");
        const bool scl = same(tok[1], "SCL");
        bool ok = (sda || scl) && ((ntok == 3U) || (ntok == 5U)) && parse_u32(tok[2], &ms);
        if (ok && (ntok == 5U))
        {
            ok = same(tok[3], "EDGES") && parse_u32(tok[4], &edges) && (edges >= 1U);
        }
        if (!ok)
        {
            reply_err(c, "usage: HOLD SDA|SCL <ms 1..500> [EDGES <n 1..255>]");
            return;
        }
        const char *why = hold_start(sda ? TARGET_PIN_SDA : TARGET_PIN_SCL, ms, edges);
        if (why != NULL)
        {
            reply_err(c, why);
            return;
        }
        Line_t l;
        ln_start(&l, "OK HOLD line=");
        ln_str(&l, sda ? "SDA" : "SCL");
        ln_str(&l, " ms=");
        ln_u32(&l, ms);
        ln_str(&l, " edges=");
        ln_u32(&l, edges);
        ln_send(&l);
        return;
    }
    reply_err(c, "unknown command");
}
