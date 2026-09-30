/**
  ******************************************************************************
  * @file    gpio_selftest.h
  * @brief   00_gpio selftest (preset selftest-gpio): entry points, the SRAM4
  *          block the two cores coordinate through, and the tests both cores
  *          run (Common/Src/gpio_selftest_common.c).
  *
  *          Built only with GPIO_DRIVER_SELFTEST. Procedure, wiring and the
  *          meaning of every test: 01_shared_drivers/00_gpio/README.md.
  ******************************************************************************
  */

#ifndef GPIO_SELFTEST_H
#define GPIO_SELFTEST_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32h7xx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Entry points ---------------------------------------------------------- */
void GPIO_Selftest_CM7_PreRelease(void);   /* CPU1, before HSEM_BM_BootReleaseCpu2 */
void GPIO_Selftest_CM7_Run(void);          /* CPU1, from a FreeRTOS task           */
void GPIO_Selftest_CM4_Run(void);          /* CPU2, after Shared_GpioLockInstall()  */

/* ---- Tiers under test ---------------------------------------------------------- */
typedef enum
{
    GST_TIER_BM = 0,
    GST_TIER_LL,
    GST_TIER_HAL,
    GST_TIER_COUNT
} GstTier_t;

/* ---- Race methods (T15) ---------------------------------------------------------- */
typedef enum
{
    GST_RACE_RAW = 0,        /* hand-written RMW, no protection: the hazard (control) */
    GST_RACE_BM,
    GST_RACE_LL,
    GST_RACE_HAL,
    GST_RACE_HAL_IN_SECTION, /* raw HAL_GPIO_Init between SectionEnter/Exit (MSP pattern) */
    GST_RACE_HAL_RAW         /* raw HAL_GPIO_Init, no section: one unprotected writer   */
} GstRace_t;

typedef struct
{
    uint32_t iters;
    uint32_t corrupt;        /* own pin wrong after a call, or the other pin invalid */
    uint32_t status_err;     /* driver status other than OK                           */
    uint32_t overlap;        /* iterations done while the other side was in its loop  */
    uint32_t locked;         /* successful lock() calls through the counting shim     */
    uint32_t hold_max;       /* cycles from lock() returning to unlock(), max         */
    uint32_t cycles;         /* duration of the loop, in this core's cycles           */
} GstRaceResult_t;

/* ---- Results of the tests both cores run ----------------------------------------- */
typedef struct
{
    uint32_t cases;          /* refusals checked                           */
    uint32_t fail;           /* wrong status, or the snapshot changed      */
    uint32_t ctrl_changed;   /* the valid control call changed the snapshot */
} GstT1Result_t;

typedef struct
{
    uint32_t cases[GST_TIER_COUNT];       /* valid configurations applied            */
    uint32_t rejected[GST_TIER_COUNT];    /* analog + pull, refused as expected      */
    uint32_t diff_rm[GST_TIER_COUNT];     /* image differs from the RM field table   */
    uint32_t diff_hal[GST_TIER_COUNT];    /* image differs from raw HAL_GPIO_Init (BM, LL only) */
    uint32_t odr_bad[GST_TIER_COUNT];     /* ODR differs from the initial level      */
    uint32_t readcfg_bad;                 /* BM/LL ReadConfig differs from the registers */
    uint32_t legacy_bad;                  /* ConfigAlternate/ConfigAnalog bits differ */
    uint32_t hal_g7_pupdr_kept;           /* raw HAL, analog + PULLUP: PUPDR not written (G7 shown) */
    uint32_t ctrl_detected;               /* the altered expected image was detected (1 = yes) */
} GstT2Result_t;

typedef struct
{
    uint32_t checks;         /* individual assertions evaluated */
    uint32_t fail;
    uint32_t busy_calls;     /* calls that returned BUSY with the deny hook */
    uint32_t ctrl_wrote;     /* the allow hook control wrote (1 = yes) */
    uint32_t swap_ctrl;      /* A4 control: the counters tell the two hooks apart (1 = yes) */
} GstTbResult_t;

typedef struct
{
    uint32_t nohook_min[GST_TIER_COUNT];
    uint32_t nohook_max[GST_TIER_COUNT];
    uint32_t hook_min[GST_TIER_COUNT];
    uint32_t hook_max[GST_TIER_COUNT];
} GstB1Result_t;

/* ---- SRAM4 block (outside g_shared, fixed address) ------------------------------ *
 * 0x3800A000: inside the RAM_SHARED region of both linker scripts (0x38008000,
 * 32 KB, non-cacheable on the CM7 through MPU region 7) and 8 KB above the start
 * of .shared_ram, which holds g_shared (under 200 B). Fixed rather than linked,
 * so both images agree on it without depending on object order.               */
#define GST_AREA_ADDR        0x3800A000UL
#define GST_AREA_MAGIC       0x47535431UL   /* "GST1" */

typedef enum
{
    GST_CMD_NONE = 0,
    GST_CMD_T1,
    GST_CMD_T2,
    GST_CMD_TB,
    GST_CMD_B1,
    GST_CMD_RACE_ARM,       /* arg0 = GstRace_t, arg1 = iterations; waits for go */
    GST_CMD_SHIM_ON,        /* install the counting shim around the production hook */
    GST_CMD_SHIM_OFF,       /* back to the production hook                          */
    GST_CMD_DONE
} GstCmd_t;

typedef struct
{
    volatile uint32_t magic;
    volatile uint32_t session;
    volatile char     m4_rev[24];      /* FW_GIT_REV of the CPU2 image      */
    volatile uint32_t m4_ready;        /* = session once CPU2 serves commands */

    volatile uint32_t cmd;
    volatile uint32_t arg0;
    volatile uint32_t arg1;
    volatile uint32_t cmd_seq;         /* CPU1 increments after writing cmd/args */
    volatile uint32_t ack_seq;         /* CPU2 copies cmd_seq when done          */
    volatile uint32_t armed_seq;       /* RACE_ARM: CPU2 copies cmd_seq when armed */
    volatile uint32_t m4_timeout;      /* CPU2 gave up waiting for a command      */

    /* T15 */
    volatile uint32_t go;              /* CPU1 sets it to start both loops        */
    volatile uint32_t in_loop[2];      /* [0] CPU1, [1] CPU2                      */
    volatile uint32_t locked_calls;    /* incremented INSIDE the lock by both cores */
    volatile uint32_t hold_start[2];

    /* CPU2 results */
    GstT1Result_t     m4_t1[GST_TIER_COUNT];
    GstT2Result_t     m4_t2;
    GstTbResult_t     m4_tb;
    GstB1Result_t     m4_b1;
    GstRaceResult_t   m4_race;

    /* T15 (R2), appended so no existing field moves: the seed of the current
       phase. CPU1 writes it before GST_CMD_RACE_ARM; both loops seed their
       jitter from it (gst_race_run). */
    volatile uint32_t race_seed;

    /* R3-a, appended: FW_BUILD_ID of the CPU2 image (generated/fw_build_id.h),
       written with m4_rev before m4_ready. */
    volatile char     m4_build_id[24];
} GstArea_t;

_Static_assert(sizeof(GstArea_t) <= 0x1000U, "GstArea_t no longer fits its 4 KB slot");

#define GST_AREA   ((GstArea_t *)GST_AREA_ADDR)

/* ---- One pin request, in gpio_bm field values ------------------------------------ */
typedef struct
{
    GPIO_TypeDef *port;
    uint32_t pin, mode, otype, speed, pull, af, level;
    bool     allow_dbg;
} GstPinReq_t;

#define GST_HAL_ERROR   100U   /* HAL_ERROR in the status space of gst_apply/gst_deinit */

/* Applies the request through one tier. Returns the GpioBm_Status_t for BM and
   LL; for HAL, GPIO_BM_OK, GPIO_BM_ERR_BUSY or GST_HAL_ERROR. */
uint32_t    gst_apply(GstTier_t t, const GstPinReq_t *c);
uint32_t    gst_deinit(GstTier_t t, GPIO_TypeDef *port, uint32_t pin);
void        gst_raw_config(const GstPinReq_t *c);     /* test-side RMW, no protection */
bool        gst_pin_is(const GstPinReq_t *c);         /* the five fields read back equal */
GstPinReq_t gst_race_cfg(uint32_t pin, uint32_t which);

/* ---- Tests both cores run (gpio_selftest_common.c) -------------------------------- */
void     gst_t1_run(GstTier_t tier, GstT1Result_t *r);
void     gst_t2_run(GstT2Result_t *r);
void     gst_tb_run(GstTbResult_t *r);
void     gst_b1_run(GstB1Result_t *r);
void     gst_race_prepare(void);                      /* both race pins to state A (CPU1) */
void     gst_race_run(GstRace_t m, uint32_t iters, GstRaceResult_t *r);
void     gst_shim_install(bool on);
uint32_t gst_core_index(void);                        /* 0 on CPU1, 1 on CPU2 */

/* Race pins: PF10 for CPU1, PF11 for CPU2 (not wired on the bench). */
#define GST_RACE_PORT        GPIOF
#define GST_RACE_PIN_CM7     10U
#define GST_RACE_PIN_CM4     11U

#ifdef __cplusplus
}
#endif

#endif /* GPIO_SELFTEST_H */
