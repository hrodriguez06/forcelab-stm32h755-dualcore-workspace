/**
  ******************************************************************************
  * @file    i2c_bm.h
  * @brief   I2C1-I2C4 (master, 7-bit addresses), bare-metal tier.
  *
  *          Driver tier: *_bm. Direct register access only: stm32h7xx.h,
  *          <stdint.h> and <stdbool.h>, no ST HAL and no LL headers (C8), no
  *          state in RAM (C7). Every silicon fact the driver needs (RCC enable
  *          bit, kernel clock field, SYSCFG_PMCR bit, DMAMUX requests, interrupt
  *          numbers) lives in one table of capabilities per instance in i2c_bm.c,
  *          each row citing RM0399 rev 4 (C1).
  *
  *          Scope (05_i2c plan, revision 2): master, 7-bit addresses 0x08..0x77;
  *          write, read, write + repeated start + read, and the address probe (a
  *          write of 0 bytes); 1 to 65 535 bytes per transfer (NBYTES in chunks of
  *          255 with RELOAD); Sm, Fm and Fm+ up to 1 MHz; analog and digital
  *          filters; the SCL-low timeout (TIMEOUTA, TIDLE = 0); by polling, by
  *          interrupt (state in the caller's I2cBm_Xfer_t) and by DMA (streams
  *          programmed by the caller); bus recovery through the caller's pin
  *          functions. Slave, 10-bit addresses, SMBus/PMBus, PEC, general call,
  *          wakeup from Stop and NOSTRETCH = 1 are not in the API.
  *
  *          Contract:
  *            - every parameter is checked before the first access, RCC and
  *              SYSCFG included, in a fixed order: NULL, instance, capability,
  *              field range, state (ERR_BUSY, then ERR_BUS) (C6); a refusal
  *              writes nothing;
  *            - a write the hardware would ignore is refused with ERR_BUSY:
  *              TIMINGR, ANFOFF, DNF or NOSTRETCH with PE = 1 (RM0399
  *              50:2505-2529, 2808), CR2 with START = 1 (50:2620-2674); every
  *              configuration write is read back (ERR_VERIFY) (C12);
  *            - in RCC only this core's enable bank is written, never a reset
  *              register; the kernel clock fields are only read; SYSCFG_PMCR
  *              only through SetFastModePlus, one bit read-modify-written with
  *              PRIMASK saved and restored and read back; ICR is written once
  *              with the bits to clear, never read-modify-written; the
  *              read-modify-writes of CR1 and CR2 run with PRIMASK saved and
  *              restored (C10);
  *            - every wait is bounded by a number of DWT cycles the caller
  *              passes, and ends if CYCCNT stops (more than 1000 equal reads)
  *              or does not run (C13);
  *            - a write starts with its first byte in TXDR before START (ES0445
  *              2.19.6), with DMA too; BERR in master mode is cleared and the
  *              transfer goes on (ES0445 2.19.2); NACK leaves the STOP the
  *              hardware sends (50:1192-1194) and returns ERR_NACK;
  *            - CalcTiming is pure, 64-bit integer arithmetic, the plan's
  *              "Reglas de calculo" (C15);
  *            - no interrupt handler, no callback, no interrupt controller call:
  *              I2C_BM_GetIRQn only returns the numbers (C14).
  *
  *          ES0445 2.19.1 (data sampled wrong when tSU;DAT is shorter than one
  *          kernel clock period) is checked by CalcTiming only, which knows the
  *          kernel frequency and the mode: a TIMINGR the caller computes itself
  *          is not checked against it (I2cBm_Config_t has no kernel frequency).
  *
  *          Pins (AF, open-drain, no pull, C11) and DMA streams are the
  *          caller's. With PE = 0 the peripheral lets go of SCL and SDA
  *          (50:429-431). Ownership: one core per instance; the driver takes no
  *          cross-core lock.
  ******************************************************************************
  */

#ifndef I2C_BM_H
#define I2C_BM_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32h7xx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Status ------------------------------------------------------------------- */
typedef enum
{
    I2C_BM_OK = 0,
    I2C_BM_ERR_PARAM,        /*!< refused before any peripheral access (C6)                     */
    I2C_BM_ERR_UNSUPPORTED,  /*!< not available on this instance                                */
    I2C_BM_ERR_RANGE,        /*!< no TIMINGR meets the mode with this kernel clock (2.19.1)     */
    I2C_BM_ERR_BUSY,         /*!< the hardware would ignore the write in this state (C12)       */
    I2C_BM_ERR_VERIFY,       /*!< read-back differs (C12)                                        */
    I2C_BM_ERR_TIMEOUT,      /*!< max_cycles expired or CYCCNT stopped (C13)                    */
    I2C_BM_ERR_NACK,         /*!< address or data not acknowledged (STOP sent by hardware)      */
    I2C_BM_ERR_ARLO,         /*!< arbitration lost                                               */
    I2C_BM_ERR_BUS,          /*!< bus not free (BUSY), SCL held low (TIMEOUT), or not recovered  */
    I2C_BM_PENDING,          /*!< interrupt transfer running (I2cBm_Xfer_t.status)               */
    I2C_BM_ERR_HAL           /*!< i2c_hal only                                                   */
} I2cBm_Status_t;

/* ---- Capabilities: one row per instance (plan rule 2) ----------------------------- */
#define I2C_BM_KSEL_I2C123  0U   /* RCC_D2CCIP2R.I2C123SEL [13:12] */
#define I2C_BM_KSEL_I2C4    1U   /* RCC_D3CCIPR.I2C4SEL    [9:8]   */
typedef struct { uint8_t kernel_select; uint8_t dmamux; } I2cBm_Caps_t;   /* dmamux 1 or 2 (BDMA) */

/** true only for the exact base address of I2C1-I2C4 (I2C1 + 4 is not an I2C). */
bool           I2C_BM_IsValidInstance(const I2C_TypeDef *i2c);
I2cBm_Status_t I2C_BM_GetCaps(const I2C_TypeDef *i2c, I2cBm_Caps_t *out);

/* ---- Clock and Fm+ ------------------------------------------------------------- */
I2cBm_Status_t I2C_BM_EnableClock(I2C_TypeDef *i2c);     /* this core's RCC bank, read back */
I2cBm_Status_t I2C_BM_DisableClock(I2C_TypeDef *i2c);    /* this core's bank only (9.5.10)  */
I2cBm_Status_t I2C_BM_GetKernelSource(const I2C_TypeDef *i2c, uint32_t *field);   /* raw I2CxSEL, read only */
/** SYSCFG_PMCR.I2CxFMP (13:122-137): one bit read-modify-written under PRIMASK and
    read back. ERR_BUSY: the SYSCFG clock is off in both RCC banks. The PBxFMP bits
    of the pins are the project's. */
I2cBm_Status_t I2C_BM_SetFastModePlus(I2C_TypeDef *i2c, bool enable);

/* ---- Pure arithmetic (C15, plan "Reglas de calculo") ------------------------------ */
#define I2C_BM_SPEED_SM   0U     /* <= 100 kHz  */
#define I2C_BM_SPEED_FM   1U     /* <= 400 kHz  */
#define I2C_BM_SPEED_FMP  2U     /* <= 1000 kHz */
typedef struct
{
    uint32_t kernel_hz;
    uint32_t scl_hz;         /*!< 1..1 000 000; the mode follows from it                      */
    uint32_t rise_ns;        /*!< bus t_r, <= the mode's maximum (50:379-383)                 */
    uint32_t fall_ns;        /*!< bus t_f, idem                                                */
    bool     analog_filter;  /*!< ANFOFF = 0                                                   */
    uint32_t dnf;            /*!< 0..15                                                        */
} I2cBm_TimingIn_t;
typedef struct { uint32_t timingr; uint32_t mode; uint32_t scl_hz_model; } I2cBm_TimingOut_t;
/** The first PRESC 0..15 whose SCLDEL, SDADEL window, SCLL and SCLH meet the mode
    (tables 405/407, 50:159-171, 319-342); scl_hz_model is the frequency the model of
    50:977-989 predicts, never above scl_hz. ERR_PARAM: NULL, kernel_hz 0, scl_hz 0 or
    > 1 000 000, dnf > 15. ERR_RANGE: t_r or t_f above the mode's maximum, kernel below
    the minimum of ES0445 2.19.1 (4, 10, 20 MHz), or no PRESC fits. */
I2cBm_Status_t I2C_BM_CalcTiming(const I2cBm_TimingIn_t *in, I2cBm_TimingOut_t *out);

/* ---- Configuration -------------------------------------------------------------- */
typedef struct
{
    I2C_TypeDef *i2c;
    uint32_t     timingr;        /*!< from CalcTiming (or the caller's own); bits 27:24 = 0  */
    bool         analog_filter;
    uint32_t     dnf;            /*!< 0..15                                                   */
    uint32_t     timeout_scl_low;/*!< 0 = off; else TIMEOUTA + 1 (1..4096), TIDLE = 0, TIMOUTEN:
                                      SCL low for timeout_scl_low x 2048 kernel clocks is a
                                      TIMEOUT (50:1793-1800), which ends a transfer with
                                      ERR_BUS and a STOP from the hardware                    */
} I2cBm_Config_t;
/** The checks of Init, in the C6 order, nothing written. ERR_PARAM: NULL, instance,
    dnf > 15, timeout_scl_low > 4096, TIMINGR reserved bits set. ERR_BUSY: PE = 1 with
    another TIMINGR, filter or TIMEOUTR in force (the hardware would ignore them), or
    with a transfer running (START/STOP pending, an interrupt or DMA enable set). */
I2cBm_Status_t I2C_BM_CheckConfig(const I2cBm_Config_t *cfg);
/** Plan rule 3 (50:404-425, 441-445): PE = 0 read back (the software reset, at least
    three APB cycles), ANFOFF/DNF, TIMINGR, TIMEOUTR (TIMEOUTA with TIMOUTEN = 0,
    50:2845), OAR1 = OAR2 = 0, CR2 = 0, CR1 with NOSTRETCH = 0 and every interrupt and
    DMA enable 0, PE = 1; read-back of CR1, TIMINGR, TIMEOUTR, OAR1/OAR2 = 0. ERR_BUS
    when ISR.BUSY = 1 after PE = 1 (the bus is not free: I2C_BM_RecoverBus). With PE = 1
    and the same configuration in force, Init is a software reset. */
I2cBm_Status_t I2C_BM_Init(const I2cBm_Config_t *cfg);
/** Plan rule 4: CR1 interrupt and DMA enables 0, PE = 0 (SCL and SDA let go) read
    back, CR2, TIMINGR, TIMEOUTR, OAR1, OAR2 at reset, ICR = every flag; read-back of
    the reset image (ISR = 0x00000001). No RCC or SYSCFG write. */
I2cBm_Status_t I2C_BM_Deinit(I2C_TypeDef *i2c);

/* ---- Blocking transfers (7-bit address 0x08..0x77; n 0..65 535, 0 only for Write) ---- */
/** The checks of a transfer of ntx bytes written then nrx read (ntx = nrx = 0: the
    address probe), in the C6 order, nothing written. ERR_PARAM: instance, addr7
    outside 0x08..0x77, ntx or nrx > 65 535. ERR_BUSY: PE = 0 (no Init), START or STOP
    pending, an interrupt or DMA enable set. ERR_BUS: ISR.BUSY = 1 (rule 5). */
I2cBm_Status_t I2C_BM_CheckTransfer(const I2C_TypeDef *i2c, uint32_t addr7, uint32_t ntx, uint32_t nrx);
/** n = 0 is the address probe: START, address, STOP (ERR_NACK when nobody answers).
    tx may be NULL only with n = 0. max_cycles >= 1 bounds the whole call; on expiry
    the instance is software-reset (PE = 0, read back, PE = 1) and ERR_TIMEOUT. */
I2cBm_Status_t I2C_BM_Write(I2C_TypeDef *i2c, uint32_t addr7, const uint8_t *tx, uint32_t n, uint32_t max_cycles);
/** n 1..65 535 (0 is ERR_PARAM). */
I2cBm_Status_t I2C_BM_Read(I2C_TypeDef *i2c, uint32_t addr7, uint8_t *rx, uint32_t n, uint32_t max_cycles);
/** ntx and nrx 1..65 535: the write ends with AUTOEND = 0 and TC, then a repeated
    start begins the read (50:544-549, 1185-1188). */
I2cBm_Status_t I2C_BM_WriteRead(I2C_TypeDef *i2c, uint32_t addr7, const uint8_t *tx, uint32_t ntx,
                                uint8_t *rx, uint32_t nrx, uint32_t max_cycles);   /* repeated start */

/* ---- Interrupt transfers (state in the caller's struct, C7) --------------------- */
typedef struct
{
    uint32_t                 addr7;
    const uint8_t           *tx;   uint32_t ntx;
    uint8_t                 *rx;   uint32_t nrx;
    uint32_t                 done_tx, done_rx;     /*!< driver-maintained: bytes the target
                                                        acknowledged, bytes read           */
    volatile I2cBm_Status_t  status;               /*!< I2C_BM_PENDING until the end */
} I2cBm_Xfer_t;
/** The checks of CheckTransfer (and x, tx and rx not NULL where they are used), then
    CR2 without START, the interrupt enables, START (plan rule 7). The first byte of a
    write is in TXDR before START (2.19.6). */
I2cBm_Status_t I2C_BM_StartIt(I2C_TypeDef *i2c, I2cBm_Xfer_t *x);
/** From both the event and the error vector of the instance: serves TXIS, RXNE, TCR,
    TC, STOPF, NACKF and the errors; true when x->status is final, and then every
    interrupt enable of CR1 is 0 again. */
bool           I2C_BM_XferIrq(I2C_TypeDef *i2c, I2cBm_Xfer_t *x);

/* ---- DMA (the caller owns streams and channels, 02_dma) ------------------------- */
/** n 2..65 535: buf[0] goes to TXDR before START (2.19.6); the caller's TX stream,
    enabled before this call, sends buf[1..n-1] (n - 1 items). TXDMAEN is set before
    START (50:2300-2337). */
I2cBm_Status_t I2C_BM_DmaStartWrite(I2C_TypeDef *i2c, uint32_t addr7, const uint8_t *buf, uint32_t n);
/** n 1..65 535; the caller's RX stream (n items) is enabled first, then RXDMAEN and
    START here. */
I2cBm_Status_t I2C_BM_DmaStartRead(I2C_TypeDef *i2c, uint32_t addr7, uint32_t n);
/** The end of a DMA transfer of n bytes (the n of DmaStartWrite/DmaStartRead): every
    TCR is served with the next chunk of NBYTES (a local count, no RAM state), then
    STOPF (OK), NACKF (ERR_NACK, after the STOP), ARLO, TIMEOUT (ERR_BUS). On expiry
    of max_cycles the instance is software-reset and ERR_TIMEOUT; the caller then
    stops its streams. (Plan revision 2 had WaitEnd(i2c, max_cycles): with more than
    255 bytes the reloads need n, approved 2026-10-07.) */
I2cBm_Status_t I2C_BM_WaitEnd(I2C_TypeDef *i2c, uint32_t n, uint32_t max_cycles);
/** After the caller stopped its streams: TXDMAEN = RXDMAEN = 0, CR2 = 0, TXDR flushed,
    every flag cleared; read back. */
I2cBm_Status_t I2C_BM_DmaEnd(I2C_TypeDef *i2c);
I2cBm_Status_t I2C_BM_GetDmaRequest(const I2C_TypeDef *i2c, bool tx, uint32_t *request_id);
I2cBm_Status_t I2C_BM_GetIRQn(const I2C_TypeDef *i2c, IRQn_Type *ev, IRQn_Type *er);

/* ---- Bus recovery (plan rule 9): the caller moves the pins; the driver times it ---- */
typedef struct
{
    void  (*scl_write)(void *ctx, bool high);   /*!< open drain: false = drive low, true = release */
    void  (*sda_write)(void *ctx, bool high);
    bool  (*scl_read)(void *ctx);
    bool  (*sda_read)(void *ctx);
    void   *ctx;
    uint32_t half_period_cycles;                /*!< CPU cycles, >= 1 (5 us at Sm is safe)        */
} I2cBm_Recovery_t;
/** With the instance at PE = 0 and the pins in GPIO open-drain (the caller's): if SDA
    reads high, nothing is driven and *pulses = 0. Otherwise up to 9 SCL pulses until
    SDA reads high, then a STOP (SDA low while SCL is high, then SDA high), and both
    lines must read high. ERR_BUS: SCL does not rise when released (someone holds it),
    or SDA still low after 9 pulses, or the lines are not high at the end. *pulses =
    pulses given. ERR_PARAM: NULL, half_period_cycles or max_cycles 0. */
I2cBm_Status_t I2C_BM_RecoverBus(const I2cBm_Recovery_t *r, uint32_t max_cycles, uint32_t *pulses);

/* ---- Errors --------------------------------------------------------------------- */
#define I2C_BM_ERRF_BERR     I2C_ISR_BERR       /* bit positions of ISR / ICR */
#define I2C_BM_ERRF_ARLO     I2C_ISR_ARLO
#define I2C_BM_ERRF_OVR      I2C_ISR_OVR
#define I2C_BM_ERRF_TIMEOUT  I2C_ISR_TIMEOUT
uint32_t       I2C_BM_TakeErrors(I2C_TypeDef *i2c);   /* BERR/ARLO/OVR/TIMEOUT; one ICR write */

#ifdef __cplusplus
}
#endif

#endif /* I2C_BM_H */
