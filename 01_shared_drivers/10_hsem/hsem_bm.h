/**
  ******************************************************************************
  * @file    hsem_bm.h
  * @brief   Bare-Metal Hardware Semaphore (HSEM) driver for STM32H755.
  *          Provides 1-step lock with self-collision detection, 2-step lock,
  *          honest release, cycle-bounded timeout, notifications, and recovery.
  *
  * Reference: RM0399 Chapter 11 (HSEM), Chapter 9 (RCC AHB4ENR banking).
  ******************************************************************************
  */

#ifndef HSEM_BM_H
#define HSEM_BM_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32h7xx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Silicon facts (C1) ------------------------------------------------- */
#define HSEM_BM_NUM_SEMAPHORES   32U
#define HSEM_BM_COREID_CPU1      3U     /* RM0399 table 95: bus master 0, Cortex-M7 */
#define HSEM_BM_COREID_CPU2      1U     /* RM0399 table 95: bus master 1, Cortex-M4 */
#define HSEM_BM_PROCID_MAX       255U
#define HSEM_BM_SEM_MASK(id)     (1UL << (id))

typedef enum
{
    HSEM_BM_OK = 0,         /*!< Locked by the caller / done                                        */
    HSEM_BM_BUSY,           /*!< Held by the other core                                             */
    HSEM_BM_BUSY_SELF,      /*!< Held by THIS core: other PROCID, other context, or re-entry (S1)   */
    HSEM_BM_TIMEOUT,        /*!< Still held when max_cycles elapsed                                 */
    HSEM_BM_ERR_PARAM,      /*!< Refused before any peripheral access (C6)                          */
    HSEM_BM_ERR_NOT_OWNER,  /*!< Release refused: the caller does not hold it; nothing written (C11) */
    HSEM_BM_ERR_TIMEBASE,   /*!< DWT CYCCNT not running: a bounded wait is impossible (C13)         */
    HSEM_BM_ERR_CORE        /*!< CORE_CM4 build flag disagrees with SCB->CPUID, or no Init (S9)     */
} HsemBm_Status_t;

typedef struct
{
    bool     locked;
    uint32_t core_id;       /*!< HSEM_BM_COREID_* ; 0 when free */
    uint32_t proc_id;       /*!< 0 for a 1-step lock            */
} HsemBm_Owner_t;

/* ---- Setup, per core ---------------------------------------------------- */
HsemBm_Status_t HSEM_BM_Init(void);            /* HSEMEN (banked, read-back), core check (S9); idempotent */
HsemBm_Status_t HSEM_BM_CheckInit(void);       /* Check HSEMEN clock and core identity without state */
bool            HSEM_BM_TimebaseStart(void);   /* DWT CYCCNT on; core-local; true if CYCCNT advances */
uint32_t        HSEM_BM_GetCoreId(void);       /* this core's COREID, as verified by Init */

/* ---- Lock --------------------------------------------------------------- */
HsemBm_Status_t HSEM_BM_FastTake(uint32_t id);                         /* 1-step, PROCID 0 */
HsemBm_Status_t HSEM_BM_Take(uint32_t id, uint32_t proc_id);           /* 2-step, proc_id 1..255 */
HsemBm_Status_t HSEM_BM_TakeTimeout(uint32_t id, uint32_t proc_id,     /* proc_id 0 -> 1-step */
                                    uint32_t max_cycles);
HsemBm_Status_t HSEM_BM_TakeTimeoutEx(uint32_t id, uint32_t proc_id, uint32_t max_cycles,
                                      void (*relax)(void *ctx), void *ctx);
HsemBm_Status_t HSEM_BM_Release(uint32_t id, uint32_t proc_id);        /* ownership checked first */

/* ---- State: reads HSEM_Rx only, never HSEM_RLRx (C11) ------------------- */
bool            HSEM_BM_IsLocked(uint32_t id);
bool            HSEM_BM_GetOwner(uint32_t id, HsemBm_Owner_t *out);
uint32_t        HSEM_BM_GetLockedMask(uint32_t core_id);               /* which IDs that core holds */

/* ---- Recovery, RM0399 §11.3.6 ------------------------------------------- */
bool            HSEM_BM_SetClearKey(uint16_t key);                     /* key 0 refused */
HsemBm_Status_t HSEM_BM_ReleaseAllOfCore(uint32_t core_id, uint16_t key); /* key 0 refused */

/* ---- Notification: the calling core's line only ------------------------- */
bool            HSEM_BM_NotifyEnable(uint32_t sem_mask);   /* CnICR first, then CnIER, under PRIMASK */
bool            HSEM_BM_NotifyDisable(uint32_t sem_mask);
uint32_t        HSEM_BM_NotifyTake(void);   /* CnMISR -> clear those CnIER bits, write CnICR, DSB */
uint32_t        HSEM_BM_GetRawStatus(void); /* CnISR */
IRQn_Type       HSEM_BM_GetIRQn(void);      /* HSEM1_IRQn on CM7, HSEM2_IRQn on CM4 */
HsemBm_Status_t HSEM_BM_TakeOrNotify(uint32_t id, uint32_t proc_id);  /* §11.3.7: OK, or BUSY with notify armed */

/* ---- Boot handshake (F2) ------------------------------------------------ */
typedef struct
{
    uint32_t sem_id;                     /*!< From the project (shared_memory.h), never from the driver */
    bool   (*ready)(void *ctx);          /*!< Extra condition, e.g. SHARED_MAGIC; NULL = flag only        */
    void    *ctx;
    volatile uint32_t *wake_count;       /*!< Optional: incremented on every wake from WFE (B2)         */
} HsemBm_BootWait_t;

HsemBm_Status_t HSEM_BM_BootWaitForCpu1(const HsemBm_BootWait_t *cfg);           /* CPU2; no timeout (C13) */
HsemBm_Status_t HSEM_BM_BootWaitCpu2Parked(uint32_t max_cycles);                 /* CPU1: D2CKRDY -> 0 */
HsemBm_Status_t HSEM_BM_BootReleaseCpu2(uint32_t sem_id, uint32_t max_cycles);   /* CPU1: until D2CKRDY = 1 */

#ifdef __cplusplus
}
#endif

#endif /* HSEM_BM_H */
