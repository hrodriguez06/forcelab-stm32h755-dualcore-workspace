# STM32H755 Shared GPIO Driver (`01_shared_drivers/00_gpio`)

Peripheral-First (Plan A), 3-tier shared driver for the GPIO ports of the
STM32H755ZI, `GPIOA`..`GPIOK`. It covers pin configuration (input, output,
alternate function and analog; push-pull or open-drain; pull; speed; initial
level), atomic pin operations, the `LCKR` lock, de-initialisation and read-back.
Every configuration write runs inside a section that masks the calling core's
interrupts and, when the project installs one, takes a cross-core lock. The
GPIO configuration registers have a single copy shared by both cores, so without
that lock two cores configuring pins of the same port can silently undo each
other's changes. `00_gpio` is the bottom of the driver stack: `01_led`, `04_uart`,
`07_adc` and `08_eth` link it, and the BM tier works before the clock tree,
the MPU and `HAL_Init()`.

Status: phase F1 of the implementation plan (removed, see
[section 15](#15-history)) plus the LL and HAL tiers, which the plan had
deferred (F3–F4). Validated on a
NUCLEO-H755ZI-Q (silicon Rev V, `REV_ID = 0x2003`) on 2026-09-30, in
`prj_01_template_dualcore` only, with an Analog Discovery 2. Two complete runs
at `0cd803f` pass: see [section 9](#9-validation). **EXTI and the compensation
cell (F2) are not implemented.** A code audit on 2026-09-30 (of `cc1ee88`) found
no blocking defect; its findings A1–A7 and their fixes are in
[section 12](#12-findings). The fixes were then run on the bench, where the
premise of A1 turned out wrong (an unclocked port reads all ones, not zero), and
its description was corrected. A re-audit at `1a635d8` left four open findings
(R1–R4, [section 12.3](#123-re-audit-1-r1r4-2026-09-30-at-1a635d8)), none
blocking. This driver has had no independent bench audit.

---

## 1. Tiers

| Tier | Files | CMake target | Links | ST dependency | Typical use |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Bare-Metal** | `gpio_bm.[c\|h]` | `stm32h7_driver_gpio_bm` (alias `stm32h7_driver_gpio`) | - | CMSIS only (`stm32h7xx.h`) | Both cores, boot code, fault paths, other drivers |
| **Low-Level** | `gpio_ll.[c\|h]` | `stm32h7_driver_gpio_ll` | `_gpio_bm` | `stm32h7xx_ll_gpio.h` inlines | Code written against `LL_GPIO_*` constants |
| **HAL** | `gpio_hal.[c\|h]` | `stm32h7_driver_gpio_hal` | `_gpio_bm` | `HAL_GPIO_Init`, `HAL_GPIO_LockPin` | MSP code, CubeMX-style `GPIO_InitTypeDef` |

```cmake
target_link_libraries(cm4_firmware PRIVATE stm32h7_driver_gpio_bm)   # or _ll, _hal
```

```c
#include "gpio_ll.h"    /* never "00_gpio/gpio_ll.h": the directory is on the include path */
```

- **`stm32h7_driver_gpio` is the BM tier.** It is an alias with no sources,
  kept because eight driver targets (`led`, `uart` ×3, `adc` ×3, `eth`) and
  the umbrella `stm32h7_shared_drivers`
  link that name. The umbrella does not link LL or HAL.
- **`gpio_bm` holds the only copy of** the validation (`GPIO_BM_CheckConfig`,
  `GPIO_BM_IsValidPort`), the port clock, the section and the hook. LL and HAL
  call into it, so every tier refuses the same requests with the same status,
  and one installed hook protects all three.
- **State in RAM, from the linked `.map`:** `gpio_bm` keeps at most 8 B of
  `.bss` per image: the hook pointer and the nesting counter. The `prj_01`
  production images link only the pointer (4 B). LL and HAL keep
  `.data = .bss = 0`.
- **LL needs no LL `.c` file** and no `USE_FULL_LL_DRIVER`; it never includes
  `stm32h7xx_hal.h`. **HAL compiles to nothing** without `HAL_GPIO_MODULE_ENABLED`.
- **The driver never defines** `EXTIx_IRQHandler` or `HAL_GPIO_EXTI_Callback`.

### 1.1 Feature matrix

| Feature | BM | LL | HAL |
| :--- | :---: | :---: | :---: |
| Input / output / AF / analog, PP/OD, pull, speed | yes | yes | yes |
| Initial level written with `BSRR` before `MODER` (no glitch) | yes | yes | yes (`ConfigPin`; `Init` keeps HAL semantics) |
| Everything validated before the first peripheral access | yes | yes | yes |
| PA13/PA14 (SWD) refused unless `allow_debug_pins` | yes | yes | yes (`GPIO_HAL_Init`: always refused) |
| Locked pin refused without writing; five fields read back | yes | yes | yes |
| Cross-core section + hook around every write | yes | yes | yes |
| Several pins, all validated before the first write | - | `ConfigPins` | `GPIO_HAL_Init` (mask) |
| Deinit (analog first), `ReadConfig` | yes | yes | deinit via BM |
| `LCKR` lock with read-back | yes | yes | yes |
| Pin operations, one `BSRR` store | `static inline` | `static inline` | `HAL_GPIO_WritePin/TogglePin/ReadPin` |
| EXTI, compensation cell | no | no | no (EXTI modes refused) |

---

## 2. API summary

### 2.1 Status codes

BM and LL return `GpioBm_Status_t`. HAL returns `HAL_StatusTypeDef` through
`GPIO_HAL_Status()`. The legacy BM functions keep their `bool`.

| `GpioBm_Status_t` | Meaning | HAL |
| :--- | :--- | :--- |
| `GPIO_BM_OK` | Written and read back | `HAL_OK` |
| `GPIO_BM_ERR_PARAM` | Refused before any peripheral access, including the port clock | `HAL_ERROR` |
| `GPIO_BM_ERR_DEBUG_PIN` | PA13/PA14 without `allow_debug_pins` | `HAL_ERROR` |
| `GPIO_BM_ERR_LOCKED` | The pin is frozen by `LCKR`; nothing was written | `HAL_ERROR` |
| `GPIO_BM_ERR_BUSY` | The cross-core hook did not get the lock; nothing was written | `HAL_BUSY` |
| `GPIO_BM_ERR_VERIFY` | Read-back differs from what was written | `HAL_ERROR` |
| `GPIO_BM_ERR_CONFLICT`, `_NOT_READY` | Reserved for F2 (EXTI, compensation cell); never returned | - |

### 2.2 `gpio_bm.h`

```c
/* Legacy (frozen signatures; since F1a they also refuse pull > 2, speed > 3 and misaligned ports) */
bool GPIO_BM_EnablePortClock(GPIO_TypeDef *port);
bool GPIO_BM_ConfigAlternate(GPIO_TypeDef *port, uint32_t pin_idx, uint32_t af, uint32_t pull, uint32_t speed);
bool GPIO_BM_ConfigAnalog(GPIO_TypeDef *port, uint32_t pin_idx);

/* Configuration */
GpioBm_Status_t GPIO_BM_ConfigPin(const GpioBm_PinConfig_t *cfg);
GpioBm_Status_t GPIO_BM_ConfigOutput(GPIO_TypeDef *port, uint32_t pin_idx, uint32_t otype,
                                     uint32_t speed, uint32_t pull, uint32_t initial_level);
GpioBm_Status_t GPIO_BM_ConfigInput(GPIO_TypeDef *port, uint32_t pin_idx, uint32_t pull);
GpioBm_Status_t GPIO_BM_ConfigAlternateEx(GPIO_TypeDef *port, uint32_t pin_idx, uint32_t af,
                                          uint32_t otype, uint32_t pull, uint32_t speed);
GpioBm_Status_t GPIO_BM_DeinitPin(GPIO_TypeDef *port, uint32_t pin_idx);   /* analog, AF0, PP, LOW, no pull, ODR 0 */
GpioBm_Status_t GPIO_BM_ReadConfig(const GPIO_TypeDef *port, uint32_t pin_idx, GpioBm_PinConfig_t *out);

/* Pin operations: static inline, no validation, one BSRR store (Toggle: one ODR read first) */
void GPIO_BM_Set(GPIO_TypeDef *p, uint32_t mask);      void GPIO_BM_Reset(GPIO_TypeDef *p, uint32_t mask);
void GPIO_BM_Toggle(GPIO_TypeDef *p, uint32_t mask);   void GPIO_BM_Write(GPIO_TypeDef *p, uint32_t pin_idx, uint32_t level);
void GPIO_BM_WritePort(GPIO_TypeDef *p, uint32_t mask, uint32_t value);
bool GPIO_BM_Read(const GPIO_TypeDef *p, uint32_t pin_idx);   uint32_t GPIO_BM_ReadPort(const GPIO_TypeDef *p);

/* LCKR */
GpioBm_Status_t GPIO_BM_LockPins(GPIO_TypeDef *port, uint32_t mask);
bool            GPIO_BM_IsLocked(const GPIO_TypeDef *port, uint32_t pin_idx);   /* reads only: port clock must be on */

/* Section and cross-core hook */
void            GPIO_BM_SetSharedLock(const GpioBm_SharedLock_t *lock);   /* per core; NULL = PRIMASK only; any time */
GpioBm_Status_t GPIO_BM_SectionEnter(GpioBm_Section_t *s);                /* to wrap code outside the driver */
void            GPIO_BM_SectionExit(const GpioBm_Section_t *s);

/* Validation without access (used by LL and HAL) */
bool            GPIO_BM_IsValidPort(const GPIO_TypeDef *port);
GpioBm_Status_t GPIO_BM_CheckConfig(const GpioBm_PinConfig_t *cfg);
```

Constants are the register field values: `GPIO_BM_MODE_INPUT/OUTPUT/AF/ANALOG`
(0..3), `GPIO_BM_OTYPE_PP/OD`, `GPIO_BM_PULL_NONE/UP/DOWN` (0..2) and
`GPIO_BM_SPEED_LOW..VERY_HIGH` (0..3). **`pin_idx` is an index (0..15), not a
mask.** `GPIO_BM_PIN_INDEX(GPIO_PIN_8)` gives 8 and `GPIO_BM_PIN_MASK(8)` gives
`0x100`. `GPIO_BM_PIN_INDEX(0)` gives 16, which every function refuses with
`ERR_PARAM`.

### 2.3 `gpio_ll.h`

These functions have the same semantics and return the same statuses as BM,
but take `GpioLL_PinConfig_t`, whose fields hold `LL_GPIO_*` constants.
`_Static_assert` checks that those constants equal the register field values.

```c
GpioBm_Status_t GPIO_LL_ConfigPin(const GpioLL_PinConfig_t *cfg);
GpioBm_Status_t GPIO_LL_ConfigPins(const GpioLL_PinConfig_t *cfgs, uint32_t count, uint32_t *done);
GpioBm_Status_t GPIO_LL_DeinitPin(GPIO_TypeDef *port, uint32_t pin_idx);
GpioBm_Status_t GPIO_LL_ReadConfig(const GPIO_TypeDef *port, uint32_t pin_idx, GpioLL_PinConfig_t *out);
GpioBm_Status_t GPIO_LL_LockPins(GPIO_TypeDef *port, uint32_t mask);
bool            GPIO_LL_IsLocked(const GPIO_TypeDef *port, uint32_t pin_idx);
/* static inline: GPIO_LL_Set, _Reset, _Toggle, _Write, _WritePort, _Read, _ReadPort */
```

- **`ConfigPins`** validates every entry before the first write, then gives each
  pin its own section. If entry *k* fails, pins 0..*k*−1 stay configured and
  `*done = k`.
- **`GPIO_LL_WritePort`** is one `BSRR` store. It does **not** wrap
  `LL_GPIO_WriteOutputPort`, which writes `ODR` as a whole and can undo a
  concurrent `BSRR` from the other core.
- **`IsLocked`** also checks `LCKK`. `LL_GPIO_IsPinLocked` alone looks only at
  `LCKy`, so it reports a pin as locked before the key sequence has run.

### 2.4 `gpio_hal.h`

```c
HAL_StatusTypeDef GPIO_HAL_ConfigPin(const GpioHal_PinConfig_t *cfg);           /* GPIO_MODE_*, GPIO_PULL*, ... */
HAL_StatusTypeDef GPIO_HAL_Init(GPIO_TypeDef *port, const GPIO_InitTypeDef *init); /* drop-in for HAL_GPIO_Init */
HAL_StatusTypeDef GPIO_HAL_DeinitPin(GPIO_TypeDef *port, uint32_t pin_idx);       /* = GPIO_BM_DeinitPin */
HAL_StatusTypeDef GPIO_HAL_LockPins(GPIO_TypeDef *port, uint32_t mask);
HAL_StatusTypeDef GPIO_HAL_Status(GpioBm_Status_t st);                            /* static inline mapping */
```

- **Modes:** `GPIO_MODE_INPUT`, `_OUTPUT_PP`, `_OUTPUT_OD`, `_AF_PP`, `_AF_OD`
  and `_ANALOG`. The `GPIO_MODE_IT_*` and `GPIO_MODE_EVT_*` modes are refused
  with `HAL_ERROR`.
- **`GPIO_HAL_Init`** keeps the semantics of `HAL_GPIO_Init`, which never writes
  `ODR`. For an output, set the level first with `HAL_GPIO_WritePin`, or use
  `GPIO_HAL_ConfigPin` with `initial_level`. The whole mask is validated, the
  port clocked, and every pin checked for `LCKR` before the first write (A1).
- **Pin levels:** `HAL_GPIO_WritePin`, `HAL_GPIO_TogglePin` and
  `HAL_GPIO_ReadPin` are single `BSRR` or `IDR` accesses. They are safe from
  both cores and need no wrapper.

---

## 3. Project contract

The driver contains no port, pin, HSEM ID or time limit of any project. The project
supplies them.

### 3.1 The cross-core hook (`Common/Inc/shared_memory.h`, `Common/Src/shared_memory.c`)

```c
#define BOARD_GPIO_HSEM_ID            5U      /* 1-step, taken only inside the gpio_bm section */
#define SHARED_GPIO_LOCK_US           50U     /* >= 10x the longest hold measured (B1, section 9.2) */
#define SHARED_GPIO_LOCK_CYCLES_CM7  (SHARED_GPIO_LOCK_US * 400U)
#define SHARED_GPIO_LOCK_CYCLES_CM4  (SHARED_GPIO_LOCK_US * 200U)

static bool shared_gpio_lock(void *ctx)
{
    (void)ctx;
    if (HSEM_BM_TakeTimeout(BOARD_GPIO_HSEM_ID, 0U, SHARED_GPIO_LOCK_CYCLES) == HSEM_BM_OK) { return true; }
    SHARED_GPIO_FAIL_COUNT++;                 /* g_shared.m7_/m4_gpio_lock_fail_count, own core only */
    return false;
}
static void shared_gpio_unlock(void *ctx) { (void)ctx; (void)HSEM_BM_Release(BOARD_GPIO_HSEM_ID, 0U); }
const GpioBm_SharedLock_t g_shared_gpio_lock = { shared_gpio_lock, shared_gpio_unlock, NULL };
void Shared_GpioLockInstall(void) { GPIO_BM_SetSharedLock(&g_shared_gpio_lock); }
```

- **A 1-step lock is enough.** The hook is called only at nesting level 1 and
  only with this core's interrupts masked, so at most one context per core holds
  the semaphore.
- **The timeout works inside the section.** `CYCCNT` keeps counting with
  PRIMASK set, so `TakeTimeout` stays bounded there.
- **The timeout is also the interrupt-latency budget.** `lock()` waits with this
  core's interrupts masked, so a configuration call can keep them masked for up
  to `SHARED_GPIO_LOCK_US` plus the call itself (§10). Choose the limit with the
  latency of your fastest ISR in mind, not only the measured holds.

### 3.2 Order of calls

| Core | Where (`prj_01`) | Why there |
| :--- | :--- | :--- |
| CPU1 | `CM7 main.c`: after `SystemClock_Config()` and `Shared_Init()`, before `HSEM_BM_BootReleaseCpu2` | Before the release, only CPU1 configures pins. The cycle limit assumes 400 MHz. |
| CPU2 | `CM4 main.c`: right after `HSEM_BM_Init()` and `HSEM_BM_TimebaseStart()` | Before this core's first pin configuration |

### 3.3 Writers outside the driver

The lock protects a port only if **every** writer of that port takes it.

| Writer | What to do |
| :--- | :--- |
| `HAL_GPIO_Init` in an MSP (`04_uart/uart_hal.c`, `08_eth/eth_hal.c`) | Replace it with `GPIO_HAL_Init` (same arguments), or put `GPIO_BM_SectionEnter/Exit` around each call. Phase T15 `g` shows the section works; phase `h` shows that one unprotected writer breaks the lock for everyone. |
| Raw `MODER`/`PUPDR` RMW in project code | Replace it with a driver call, or wrap it in the section |
| `01_led` (`LED_Init`, before the release) | Safe by contract: CPU2 is parked |
| `LED_PanicRed` (fault path) | Documented exception (G14): a fault handler cannot wait for a semaphore |

In `prj_01`, CPU2 configures no pin in production, and the console MSP (GPIOD)
is the only `HAL_GPIO_Init` left. Neither is migrated.

---

## 4. Rules the driver enforces

1. **Validate before the first access (C6).** Rejected with `ERR_PARAM`, before
   the port clock is touched: `NULL`, a port outside `GPIOA..GPIOK` or off the
   0x400 stride, pin > 15, mode > 3, otype > 1, pull > 2 (`PUPDR = 11` is
   reserved), speed > 3, AF > 15, level > 1, and analog with a pull. PA13/PA14
   return `ERR_DEBUG_PIN`. The checks run in a fixed order: `NULL`, port, pin,
   fields, SWD.
2. **Section (C10).** PRIMASK is saved and interrupts masked (with `DSB`/`ISB`),
   then the hook is called at nesting level 1. `SectionExit` restores the saved
   PRIMASK and never calls `__enable_irq()`, so the driver works from a fault
   handler. A refusal from the hook restores PRIMASK and the nesting counter.
   `SectionExit` releases the hook the section took, recorded in
   `GpioBm_Section_t`, so `SetSharedLock` can be called at any time (A4).
   `EnablePortClock` takes no section: `RCC_AHB4ENR` is banked per core
   (RM0399 §9.7.1).
3. **Write order (C11).** `BSRR` (output only), `OTYPER`/`OSPEEDR` (output and
   AF), `PUPDR`, `AFR` (AF only), and `MODER` last. Deinit goes the other way:
   `MODER = analog` first.
4. **Honesty (C12).** A pin whose `LCKy` bit is set with `LCKK` set returns
   `ERR_LOCKED` without writing. After every write, the five fields of the pin
   are read back and compared.
5. **`LockPins`.** The key sequence runs inside the section, as 32-bit accesses.
   On a port that is already locked, the pins already locked stay in the
   sequence and the read-back decides the result (§6). A mask whose pins are all
   locked already writes nothing.
6. **Bounded waits (C13).** The only wait is inside the hook, and the project
   bounds it.
7. **Pin operations** are single stores to `BSRR`, which is atomic
   (RM0399 §12.3.5), so they take no section. Two cores toggling the **same**
   pin can both read the same `ODR`.

---

## 5. Why not call `HAL_GPIO_*` directly

| `HAL_GPIO_*` (from `stm32h7xx_hal_gpio.c`) | Consequence | Driver |
| :--- | :--- | :--- |
| Unprotected read-modify-write of `MODER`/`OTYPER`/`OSPEEDR`/`PUPDR`/`AFR`, which both cores share | A configuration on the other core can undo this one without any error (T15 `c`: 320 906 corruptions) | Section + hook |
| Parameters checked only by `assert_param` | Invalid values reach the registers in release builds | `GPIO_BM_CheckConfig` |
| `HAL_GPIO_Init` never writes `ODR` | An output drives a stale level before the code writes the right one (T8 control: 2 transitions) | `initial_level` via `BSRR` before `MODER` |
| Analog + `GPIO_PULLUP` skips `PUPDR`; analog + `GPIO_PULLDOWN` writes it | The pull is left as it was, or enabled on an analog pin (G7, shown 6/6 in T2) | Analog with a pull is refused |
| Writes a locked pin and returns nothing | A configuration that did not happen looks successful | `ERR_LOCKED`, read-back |
| `HAL_GPIO_LockPin` checks only `LCKK` | After any earlier lock of the port, it returns `HAL_OK` whatever happened to the pins asked for | Read-back of every pin of the mask |
| `HAL_GPIO_DeInit` clears this core's `IMR1`/`EMR1` and the shared `RTSR1`/`FTSR1`/`EXTICR` of any line routed to the port, including every GPIOA line while `EXTICR` still holds its reset value | Deinit of a pin can stop the other core's EXTI | `GPIO_HAL_DeinitPin` = `GPIO_BM_DeinitPin`, which never writes EXTI |

---

## 6. Silicon facts and errata

| Rule | Mechanism | Test |
| :--- | :--- | :--- |
| RM0399 §12.3.5: `BSRR` is atomic, and set wins over reset | Pin operations through `BSRR` only | T3 (`0x3F` after a combined set+reset of PE8) |
| RM0399 §12.4.4: `PUPDR = 11` is reserved | `ERR_PARAM` | T1 |
| RM0399 §12.4.8: key sequence, word accesses; a locked pin ignores writes | `LockPins`, `ERR_LOCKED` before writing | T16 |
| In analog mode, `IDR` reads 0 | Silicon fact | T7 |
| RM0399 §9.7.1: `RCC_AHB4ENR` is banked per core | No section in `EnablePortClock`; both banks in the T1 snapshot | T1 |
| PA13/PA14 carry SWD after reset (RM0399 §12.3.2) | `ERR_DEBUG_PIN` | T1 |
| **Measured, differs from RM0399 §12.4.8** ("LCKy can only be written when LCKK is 0") | After PF7\|PF8 were locked (`LCKR = 0x00010180`), a second full sequence for PF6 gave `0x000101C0` (SWD read, 2026-09-30). The pins already locked stayed locked. The driver does not rely on either reading: it keeps the locked pins in the sequence and judges by read-back | T16 (`0x000101F0` after BM, LL and HAL each add one pin) |
| `GPIOJ`/`GPIOK` have no pads on this package, but their registers work | T16 locks pins there, where a lock can reach nothing | T16 |

**Errata (ES0445 rev 6)** that concern pins but not this code:

- **2.3.3:** a GPIO assigned to the DAC cannot be used as an output while the DAC
  drives an on-chip peripheral (PA4/PA5).
- **2.3.13:** transitions on PC13 can disturb the LSE oscillator. On the Nucleo,
  PC13 is the user button, and no test uses it.

ES0445 has no entry for `LCKR`.

---

## 7. Examples

### A. Output and input (`gpio_bm`, either core)

```c
/* CS of an SPI device on PD14: high before it becomes an output */
if (GPIO_BM_ConfigOutput(GPIOD, 14U, GPIO_BM_OTYPE_PP, GPIO_BM_SPEED_HIGH,
                         GPIO_BM_PULL_NONE, 1U) != GPIO_BM_OK) { /* refused, locked, busy or verify */ }
GPIO_BM_Reset(GPIOD, GPIO_BM_PIN_MASK(14U));   /* select */
GPIO_BM_Set(GPIOD, GPIO_BM_PIN_MASK(14U));     /* deselect */

(void)GPIO_BM_ConfigInput(GPIOC, 13U, GPIO_BM_PULL_NONE);
bool pressed = GPIO_BM_Read(GPIOC, 13U);
```

### B. I2C pins, open-drain AF (`gpio_bm`)

```c
(void)GPIO_BM_ConfigAlternateEx(GPIOB, 8U, 4U, GPIO_BM_OTYPE_OD, GPIO_BM_PULL_UP, GPIO_BM_SPEED_LOW);  /* I2C1_SCL */
(void)GPIO_BM_ConfigAlternateEx(GPIOB, 9U, 4U, GPIO_BM_OTYPE_OD, GPIO_BM_PULL_UP, GPIO_BM_SPEED_LOW);  /* I2C1_SDA */
```

### C. Several pins, all or nothing (`gpio_ll`)

```c
static const GpioLL_PinConfig_t k_bus[] = {
    { GPIOE, 8U, LL_GPIO_MODE_OUTPUT, LL_GPIO_OUTPUT_PUSHPULL, LL_GPIO_SPEED_FREQ_LOW, LL_GPIO_PULL_NO, 0U, 0U, false },
    { GPIOE, 9U, LL_GPIO_MODE_OUTPUT, LL_GPIO_OUTPUT_PUSHPULL, LL_GPIO_SPEED_FREQ_LOW, LL_GPIO_PULL_NO, 0U, 0U, false },
};
uint32_t done;
if (GPIO_LL_ConfigPins(k_bus, 2U, &done) != GPIO_BM_OK) { /* done = pins configured before the failure */ }
GPIO_LL_WritePort(GPIOE, 0x0300U, 0x0100U);   /* PE8 high, PE9 low, one BSRR store */
```

### D. An MSP that is safe on a dual-core board (`gpio_hal`)

```c
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    GPIO_InitTypeDef gpio = { .Pin = GPIO_PIN_8 | GPIO_PIN_9, .Mode = GPIO_MODE_AF_PP,
                              .Pull = GPIO_PULLUP, .Speed = GPIO_SPEED_FREQ_VERY_HIGH,
                              .Alternate = GPIO_AF7_USART3 };
    if (GPIO_HAL_Init(GPIOD, &gpio) != HAL_OK) { Error_Handler(); }   /* was HAL_GPIO_Init */
    /* ... */
}
```

### E. Code outside the driver (`gpio_bm` section)

```c
GpioBm_Section_t s;
if (GPIO_BM_SectionEnter(&s) == GPIO_BM_OK)      /* PRIMASK + the cross-core lock */
{
    HAL_GPIO_Init(GPIOB, &one_pin);               /* one pin per section keeps the masked time short */
    GPIO_BM_SectionExit(&s);
}
```

---

## 8. Test bench (Analog Discovery 2)

Board: NUCLEO-H755ZI-Q (MB1363). Instrument: an AD2 (SN `210321B5D38D`, WaveForms
SDK, driven from `tools/ad2.py` over ctypes). The WaveForms GUI must stay closed.

```text
 AD2                     NUCLEO-H755ZI-Q                      AD2 scope
 DIO0..DIO7  ─────────── PE8..PE15  (bus, one owner per step)
 DIO8        ─────────── PE3   (AD2 only; not driven by this bench)
 DIO9        ─────────── PE5   (MCU only: T8)
 DIO10       ─────────── PE4   (AD2 only; not driven by this bench)
 DIO11       ─────────── PE6   (MCU only; unused)          PF14 ──┬──── 1+   (1− to GND)
 DIO12 ───────────────►  PE2   HOST_REQ                           ├─[22k]─ GND
 DIO13 ◄───────────────  PE7   MCU_ACK                            └─[150p]─ GND
 DIO14 ◄───────────────  PA8   MCO1 (T10)                  PF15 ──┬──── 2+   (2− to GND)
 GND   ─────────────────  GND  (connected first, removed last)    ├─[22k]─ 3V3
 W1, W2, V+, V−: never connected                                 └─[150p]─ GND
```

| AD2 | MCU | Zio | Morpho | Use | Who drives |
| :--- | :--- | :--- | :--- | :--- | :--- |
| DIO0..DIO7 | PE8..PE15 | CN10-18, CN9-15, CN10-24, CN10-6, CN10-26, CN10-10, CN10-8, CN10-30 | CN12-40, 52, 47, 56, 49, 55, 51, 53 | Bus (T3, T4/T7, T0b(c)) | MCU **or** AD2, with the S5 handover |
| DIO8 | PE3 | CN9-22 | CN11-47 | EXTI line (F2) | AD2 only |
| DIO9 | PE5 | CN9-18 | CN11-50 | T8 | MCU only |
| DIO10 | PE4 | CN9-16 | CN11-48 | EXTI line (F2) | AD2 only |
| DIO11 | PE6 | CN9-20 | CN11-62 | Unused | MCU only |
| DIO12 | PE2 | CN9-14 | CN11-46 | `HOST_REQ` | AD2 only |
| DIO13 | PE7 | CN10-20 | CN12-44 | `MCU_ACK` | MCU only |
| DIO14 | PA8 | CN10-4 | CN12-23 | MCO1 (T10) | MCU only |
| Scope 1+ | PF14 | CN9-19 | CN12-50 | T5, T6, T9; 22 kΩ to GND, 150 pF to GND | MCU only |
| Scope 2+ | PF15 | CN9-21 | CN12-60 | T5, T6, T9; 22 kΩ to 3V3, 150 pF to GND | MCU only |

The pins used unwired are PF3..PF11 (T1, T2, TB, B1, T15, T16), GPIOJ and GPIOK
(T16). PF0..PF5, PF12 and PF13 reach no connector on this board. PE0 and PE1
(LD2) are never touched.

**Electrical safety.** There are no series resistors, so nothing limits the
current in a contention (DS12923 table 11: 20 mA per pin, +0 mA injection on FT
pins). A contention or an injection can damage a pin with no immediate symptom.
These rules cannot be relaxed:

| # | Rule | Where it is enforced |
| :--- | :--- | :--- |
| S1 | At most one driver per line, always. Only the bus changes direction, and only through S5 | Fixed masks on both sides (S3, S4) |
| S2 | Safe state by default: the AD2 with every DIO in input; the MCU with every wired pin in input except `MCU_ACK` (low). At open/close, between steps, on any exception, timeout or abort | `tools/ad2.py` (`FDwfDigitalIOReset`, OE read-back, `DwfParamOnClose = 1`, never 0), `try/finally`; `bench_safe()` |
| S3 | Host guard: every `OutputEnable` write goes through `ad2_set_oe(mask, bus_owner)`, which raises, after going to S2, on any bit outside {DIO8, DIO10, DIO12} plus the bus only in a "bus = AD2" step | `tools/ad2.py`; T0b(a) on a mock DLL |
| S4 | Firmware guard: every configuration of a wired pin as output, OD or AF goes through `bench_claim()`. PE0–PE4 and PE7 are always refused, the bus only in a "bus = MCU" step. The permission comes from the current step, which only `step_begin()` sets | `CM7/App/Src/gpio_selftest.c`; T0b(b) |
| S5 | Break-before-make: the bus must follow both MCU pulls (0xFF, 0x00), with the OE of the bus read back as 0, in the same step, before either side drives it | `buschk()` / `s5_released()`; T0b(c) drives it on purpose |
| S6 | At least 1 ms between one side releasing and the other driving | Explicit waits on both sides |
| S7 | The host acts only on an announced step that matches its own table; 10 s without the other side → S2 and abort, on both sides | `step_begin()` / `Session.step()`; T0b(d) |
| S8 | No output enabled before the banner `GPIO_BENCH v1 idle` of a clean `Rev` has been read | `gpio_hw_check.py` |
| S9 | AD2 on USB whenever the Nucleo is powered; GND first on, last off; never unplug or reset the Nucleo with the script running | Procedure below |
| S10 | Flash or reset only with the AD2 in S2 (the MCU puts every pin in analog meanwhile) | The script resets the board itself, with the AD2 in S2 |

Never connected to the board: W1, W2, V+, V−; any DIO to 3V3, 5V, VIN, NRST,
BOOT0 or a pin not in the table above; any component other than the four of
PF14/PF15.

**Procedure:**
1. Connect GND first, then the DIO lines.
2. Flash with the script stopped: `py tools/flash.py --preset selftest-gpio`.
3. Run `py tools/gpio_hw_check.py`. It resets the board through SWD, with the
   AD2 in S2.
4. After the verdict, stop the script, and only then reflash, reset or unwire.
5. Keep the AD2 on USB whenever the Nucleo is powered.

---

## 9. Validation

Project: `03_firmware_templates/prj_01_template_dualcore`, preset
`selftest-gpio`. Code:
- CPU1: `CM7/App/Src/gpio_selftest.c`.
- CPU2: `CM4/App/Src/gpio_selftest.c`.
- Both cores: `Common/Src/gpio_selftest_common.c`.
- Host: `tools/gpio_hw_check.py`.

The two cores coordinate through a block at `0x3800A000` (SRAM4). CPU1 opens a
new session there before it releases CPU2. The global verdict needs
`FW VERDICT=PASS` and every host check (`HOST VERDICT=PASS`). No test compares a
tier with itself: the oracles are register snapshots, a field table written from
RM0399, ST's `HAL_GPIO_Init`, private counters, and the AD2.

### 9.1 Tests (runs at `0cd803f`, identical in both unless two values are given)

| Test | What it proves | Negative control | Result |
| :--- | :--- | :--- | :--- |
| T1 | Refusals of each tier on both cores: exact status; snapshot of every clocked port and both `RCC_AHB4ENR` banks unchanged. Includes a batch with a bad third entry and a mask with PA0 next to PA13 | A valid call of the same tier changes the snapshot | 45 BM / 32 LL / 35 HAL refusals per core, 0 failures |
| T2 | 1440 configurations per tier (PF6–PF11, 4 modes × 2 types × 4 speeds × 3 pulls × AF 0/7/15), from a state with every field different, match the RM0399 field table; BM and LL also match raw `HAL_GPIO_Init`; `ODR` = initial level; BM/LL `ReadConfig` = registers; legacy calls give the `ca569b6` bits | A deliberately altered expected image is detected | 0 differences in 4320 configurations per core (1440 per tier) and in 2880 comparisons with raw `HAL_GPIO_Init`; 288 analog+pull refusals per tier; G7 shown on raw HAL 6/6 |
| TB | A refusing hook: 16 calls × PRIMASK 0/1 all return busy, nothing written, PRIMASK restored, `lock()` called once, nesting counter back to 0. Nesting: one lock for an outer section. **A4**: `SetSharedLock` to a second hook and to `NULL` inside an open section, PRIMASK 0/1: the exit releases the hook that was taken, the new hook is never released without a lock, and the next section uses the new hook (32 checks) | The same hook allowing writes; A4: swapped *before* the section, the call is locked and released on the second hook only (`swap_control=1`) | 274 checks, 0 failures, 32/32 busy, `swap_control=1`, both cores. Mutation M-A4 (exit releases the installed hook): 6 failures on each core |
| T15 | Race PF10 (CPU1) against PF11 (CPU2), same registers | Raw RMW on both cores: **320 906** corruptions, with overlap on both. Each core alone: 0 | Through the production hook (counting shim), 0 corruptions and `locked_calls` = sum of both cores in: HAL/LL, LL/HAL, BM/BM and `HAL_GPIO_Init`-in-section/BM, 200 000 calls per core each |
| T15 `h` | One writer without the section defeats the lock | - | Raw `HAL_GPIO_Init` on CPU1 against BM on CPU2: 9 944 corruptions |
| T15 `i`/`j` | A TIM17 ISR (about 20 kHz) reconfigures PF6 while the thread reconfigures PF10 | Raw RMW in both: 1 589 corruptions in 12 821 ISR entries | Thread HAL + ISR LL: 0 in 20 503 entries |
| T0a | 13 lines follow both MCU pulls; REQ/ACK work | - | 0x4FFF / 0x0000 on the AD2; `IDR` 0xFF78 / 0x0000 |
| T0b | (a) host guard refuses 8 masks before the DLL (mock); (b) `bench_claim` refuses PE0–PE4, PE7, bus, PA8, PF14; (c) the released-bus check catches a driven bus; (d) no `HOST_REQ` → MCU safe state | (a) allowed masks pass; (b) PE5 accepted; (c) the MCU drives 0x5A on purpose | All pass; (c) `False` twice at `0x5A`; (d) GPIOE `MODER[31:4]` = `0x00004000` |
| T3 | Pin operations of each tier (`WritePort`, `Set`, `Reset`, `Write`, `Toggle`) read by the AD2 | A deliberate 0x54 where the host expects 0x55 | 24 values per tier (26 for BM), 0 differences; set+reset → `0x3F`; control detected at index 25 only |
| T4 / T7 | 256 patterns driven by the AD2, read per tier by port and by bit; then analog `IDR` = 0 | Inverted pass (BM): CRC differs and equals the inverted CRC; input mode reads 0xFF | CRC32 `0x29058C73` = host for all tiers; T7 0xFF → 0x00 after `DeinitPin` |
| T5 | Pull resistances set by each tier; the scope load R<sub>IN</sub> is measured, not assumed | PF14 no pull < 0.1 V; PF15 no pull > 2.9 V; V<sub>DD</sub> agrees on both channels within 50 mV | R<sub>PU</sub> 39.1–39.4 kΩ, R<sub>PD</sub> 42.6 kΩ (30–50 kΩ, DS12923 table 59); R<sub>IN</sub> 1053.0 / 1049.2 kΩ |
| T6 | Open-drain of each tier: PF15 as an I2C line (22 kΩ to 3V3), PF14 released | PF14 push-pull high: 3.303–3.304 V | PF15 OD low 0.026–0.027 V, released 3.234–3.235 V; PF14 released 0.085–0.089 V |
| T8 | Input → output on PE5 through each tier: A, B one transition; C, D (stale `ODR`) none | Naive order (`MODER`, 1 µs, `BSRR`): a pulse | A=1 B=1 C=0 D=0 on all tiers; control 2 transitions |
| T9 | t<sub>r</sub> 10–90 % per `OSPEEDR`, 40 edges per speed and pin, 4 interleaved rounds (BM, LL, HAL, BM), Lanczos reconstruction (errata E9) | Pins in input: no edge in 0.5 s | Gate t<sub>r</sub>(LOW) − t<sub>r</sub>(VERY_HIGH) > 5σ: PF14 1.67 > 1.22 ns and 2.06 > 1.20 ns; PF15 2.53 > 0.77 ns and 2.15 > 0.83 ns |
| T10 | PA8 in AF0 through each tier outputs MCO1 = HSE/2 | PA8 in AF1: 0 edges | 4.048–4.058 MHz (4 MHz ± 2 %); `RCC_CFGR` restored |
| T16 | Locked pins refuse every tier (config, deinit, legacy) with `MODER` unchanged; a locked port gets more pins from each tier; LL on GPIOJ, HAL on GPIOK | PF6, then PF3, stay free and configurable | `LCKR` `0x00010180` → `0x000101F0`; GPIOJ/GPIOK `0x00010003` |
| T16 A1 | `GPIO_HAL_Init` on GPIOK unclocked in both banks, PK5 locked next to PK0/PK1 (`LCKR = 0x00010023`), PK3 in analog. (A) `PK3\|PK5` → `HAL_ERROR`, `MODER` and `LCKR` unchanged; (B) PK3 alone → `HAL_OK`, PK3 an output. The unclocked read is printed: the code before A1 fails (A) if it reads 0 and (B) if it reads all ones | Mutation M-A1 (no clock before the `LCKR` check): (B) `HAL_ERROR`, PK3 not written | Unclocked `LCKR` reads `0xFFFFFFFF`, `IsLocked` PK3 = PK5 = 1; (A) and (B) pass. At `8b8919a` the step still required a 0 read and failed on that premise only |
| Production | 20 warm resets of `debug` and of `release`; lock-fail counters by SWD | - | 20/20 "CPU2 alive" in each; `m7/m4_gpio_lock_fail_count` = 0 |

### 9.2 Measurements (B1)

Cycles for one `ConfigPin`, over 1000 calls with PRIMASK set around each, with no
contention (min/max):

| Tier | CM7, no hook | CM7, hook | CM4, no hook | CM4, hook |
| :--- | :--- | :--- | :--- | :--- |
| BM | 551 / 739 | 954 / 989 (2.47 µs) | 475 / 647 | 767 / 985 (4.92 µs) |
| LL | 744 / 823 | 1118 / 1141 (2.85 µs) | 622 / 749 | 1113 / 1192 (5.96 µs) |
| HAL | 1164 / 1264 | 1496 / 1559 (3.89 µs) | 1191 / 1364 | 1827 / 1875 (9.37 µs) |

The longest lock hold under contention in T15 was **801 cycles on CM7 (2.00 µs)**
and **841 cycles on CM4 (4.21 µs, HAL tier)**. `SHARED_GPIO_LOCK_US = 50` is 11.9×
the longer of the two, and the selftest fails below 10×. The first value, 20 µs,
left only 4.7×.

Flash (`.text` + `.rodata`, from the `.map`, "Discarded input sections"
excluded):
- `prj_01` release (production): `gpio_bm` 92 B on CM7 and 20 B on CM4.
- Selftest image (debug, `-Og`): `gpio_bm` 1656 B, `gpio_ll` 1246/1256 B,
  `gpio_hal` 926/928 B (CM7/CM4).

### 9.3 Evidence

All of these are in `prj_01_template_dualcore/docs/`. Each file is the unedited
output of one run.

| File | Content |
| :--- | :--- |
| `gpio_selftest_0cd803f.txt`, `_2.txt` | The two runs of §9.1, built from a clean worktree (`Rev 0cd803f` on both cores) |
| `gpio_warm_resets_{debug,release}_0cd803f.txt`, `gpio_swd_counters_0cd803f.txt` | Production regression: 20/20 each, lock-fail counters 0 |
| `gpio_map_sizes_0cd803f.txt`, `gpio_build_all_0cd803f.txt` | Sizes; `build_all.ps1 -Clean` 8/8 (compile only) |
| `gpio_dev_mutation_hal_init_no_clock_0cd803f-dirty.txt` | M-A1, `GPIO_HAL_Init` without the clock before the `LCKR` check: `T16 A1` case (B) refused |
| `gpio_dev_mutation_exit_installed_hook_0cd803f-dirty.txt` | M-A4, `SectionExit` releasing the installed hook: TB fails on both cores (6 each) |
| `gpio_dev_mutation_ll_no_section_780dddd-dirty.txt` | `gpio_ll` without its section: TB fails on both cores; T15 `d`/`e` 15 708 and 8 507 corruptions |
| `gpio_dev_mutation_hal_no_level_780dddd-dirty.txt` | `gpio_hal` without the `BSRR` level: T2 `odr_bad` 432 per core; T6_HAL and T8_HAL A–D fail |
| `gpio_selftest_8b8919a.txt` | The first run of the audit tests: all pass except `T16 A1`, whose premise (unclocked `LCKR` = 0) the silicon refuted |
| `gpio_selftest_2b9bf21*.txt`, `gpio_selftest_780dddd*.txt`, `*_780dddd.txt` | Earlier evidence (before the audit fixes); superseded |

### 9.4 Reproducing it

```powershell
.\tools\env.ps1
cd 03_firmware_templates\prj_01_template_dualcore
cmake --preset selftest-gpio; cmake --build --preset selftest-gpio
py tools/flash.py --preset selftest-gpio
py tools/gpio_hw_check.py                  # writes docs/gpio_selftest_<rev>.txt; refuses a -dirty Rev
```

### 9.5 Not exercised

- **Other projects on the board.** `prj_02`, `prj_03` and `prj_04` were only
  compiled (8/8). `prj_04` does not install the hook, so G1/G13 are still open
  there: CM4 writes PB1 by hand in `adc_acquisition.c`, and the MSPs of `eth_hal`
  and `uart_hal` call `HAL_GPIO_Init` without the section.
- **`ERR_VERIFY` from LL and HAL** on a correct build (BM returns it in T15 `h`);
  `LockPins` returning `ERR_VERIFY`.
- **`ConfigPins` failing at entry *k* > 0** after a successful validation.
- **Reconfiguring PA13/PA14** with `allow_debug_pins`.
- **Ports B, C, D, G and H** through the new API. The bench uses A, E, F, J
  and K.
- **T15 `j` covers only an LL ISR against a HAL thread.** An LL ISR against an
  LL thread is not run: mutation M1 goes undetected in that phase.
- **Silicon revisions other than V.**

---

## 10. Known limitations

- **One writer outside the driver breaks the lock for a port** (T15 `h`). §3.3
  lists the writers.
- **T9 is close to its gate on PF14:** 1.67 and 2.06 ns of separation against a
  5σ of 1.22 and 1.20 ns. At 30 MHz of bandwidth, the AD2 cannot resolve MEDIUM,
  HIGH and VERY_HIGH from each other (12.6–13.1 ns on PF14, 14.6–14.9 ns on PF15).
- **R<sub>IN</sub> of the scope once read 20.8 MΩ** in a development run, with
  V<sub>DD</sub> 66 mV low on PF15. The cause was not found. The cross-channel
  check added in `2b9bf21` fails such a run.
- **Toggle is not atomic** between two togglers of the same pin (one `ODR` read,
  then one `BSRR` store).
- **The section masks interrupts for one pin configuration.** The longest
  uncontended call measured is 9.37 µs (CM4, HAL tier, §9.2). Under contention
  it grows by the other core's hold: at most 2.00 µs measured on CM7 (so about
  11 µs on CM4), and 4.21 µs measured on CM4 (about 8 µs on CM7).
  Configure one pin per section; never loop inside one.
- **The bound on masked time is the hook's limit, not the measurements (A2).**
  `lock()` waits with interrupts masked, so the worst case is
  `SHARED_GPIO_LOCK_US` (50 µs in `prj_01`) plus the call, reached whenever the
  other core holds the semaphore longer than that.
- **A core that stops while holding the lock keeps it until reset (A2).** If a
  core takes a HardFault inside a section, or the debugger halts it there, the
  HSEM stays taken. From then on every configuration call of the other core
  masks its interrupts for the full limit and returns `ERR_BUSY`, and
  `mX_gpio_lock_fail_count` grows. Only a system reset clears it.
- **One fault-handler window is unprotected (A3).** A fault handler that
  configures a pin after preempting this core while it waited inside `lock()`
  opens a nested section: it takes no lock, and writes while the other core may
  hold it. Once `lock()` has returned, the lock is this core's and a nested
  section is covered. Same class as G14: the fault path is terminal.
- **`IsLocked` and `ReadConfig` only read (A5).** With the port unclocked in both
  cores' banks the registers read all ones (`LCKR = 0xFFFFFFFF` on Rev V,
  measured in T16 A1), so every pin reads as locked. Every
  configuration call enables the clock before it looks at `LCKR`.
- **PA13/PA14 cannot be de-initialised** through any tier: `DeinitPin` has no
  `allow_debug_pins`. `ConfigPin` with `MODE_ANALOG` and `allow_debug_pins`
  puts them in analog, but leaves `OTYPER`, `OSPEEDR`, `AFR` and `ODR` as they
  were.
- **`GPIO_HAL_Init` refuses PA13/PA14** in any mask.
- **G14:** `LED_PanicRed` writes `MODER` from a fault handler without the lock.
- **EXTI (F2) is missing:** there is no conflict detection on `EXTICR` and no
  deinit that respects the other core, and the `GPIO_MODE_IT/EVT_*` modes are
  refused. The compensation cell is missing too; `prj_01` still enables it with
  `HAL_EnableCompensationCell()`.

---

## 11. Consumers

| Consumer | Tier | Uses |
| :--- | :--- | :--- |
| `01_led/led_bm.c` | BM | `EnablePortClock`, then `MODER` directly |
| `04_uart` (all tiers) | BM | `ConfigAlternate`; `uart_hal`'s MSP also calls `EnablePortClock` + `HAL_GPIO_Init` |
| `07_adc` (all tiers) | BM | `ConfigAnalog` |
| `08_eth/eth_hal.c` | BM | `EnablePortClock` + `HAL_GPIO_Init` |
| `prj_01_template_dualcore` | BM (hook) on both cores; LL and HAL in `selftest-gpio` only | `Shared_GpioLockInstall()` |
| `prj_04_adc_dma_bm_eth` | BM | F1a selftest; hook not installed (F1b not done) |

---

## 12. Findings

### 12.1 Audit of F1 and the LL/HAL tiers (A1–A7, 2026-09-30)

A code review of `cc1ee88`: every function of the three tiers, the `prj_01`
hook, the claims about `stm32h7xx_hal_gpio.c` checked against the Cube source
`prj_01` builds with, the header freeze against `main`, the evidence of §9.3
against git, and a build with `-Wextra -Wconversion -Wsign-conversion
-Wshadow -Wcast-qual`. The fixes and their tests were run on the bench
afterwards (§9.1, §9.3).

| ID | Finding | State |
| :--- | :--- | :--- |
| A1 | `GPIO_HAL_Init` read `LCKR` before clocking the port. The audit assumed an unclocked port reads `LCKR = 0`, so a lower free pin would be written before `apply()` found the locked one. **The bench showed otherwise** (`gpio_selftest_8b8919a.txt`): on Rev V an unclocked port reads all ones (`LCKR = 0xFFFFFFFF`), so every pin looked locked and `GPIO_HAL_Init` refused **any** mask on an unclocked port, free pins included | Fixed: the port is clocked between the validation and the `LCKR` check, which is right for either reading. `T16 A1` checks both cases and passes; mutation M-A1 fails it (§9.1) |
| A2 | §10 gave the masked time as the measured holds (≈ 11 µs); the bound is the hook's limit (50 µs) plus the call. A core that faults or is halted inside a section keeps the HSEM until reset | Documented (§3.1, §10) |
| A3 | A fault handler that preempts `lock()` opens a nested section and writes without the lock | Documented (§10, `gpio_bm.h`) |
| A4 | `SetSharedLock` inside an open section made `SectionExit` call the new hook's `unlock` (or none, leaving the HSEM taken) | Fixed: `GpioBm_Section_t.taken` records the hook that was locked, and `SectionExit` releases that one. TB step A4 passes on both cores; mutation M-A4 fails it (§9.1) |
| A5 | `IsLocked` said nothing about the port clock. Unclocked, the port reads all ones on Rev V (the audit assumed 0), so every pin reads as locked | Documented (`gpio_bm.h`, `gpio_ll.h`, §10), corrected after the bench (T16 A1) |
| A6 | Stale comments in `gpio_bm.h`: legacy calls "without a lock", `@retval false` only for bad parameters, a "section 4" that no longer holds that text | Fixed in place (C2 allows comment changes) |
| A7 | `GPIO_BM_PIN_INDEX(0)` was `__builtin_ctz(0)`, undefined; `-Wcast-qual` in both `ReadConfig`; README counted seven alias users (there are eight) and did not name the cleanup commit | Fixed: `PIN_INDEX(0)` = 16, refused everywhere; casts through `uintptr_t`; README corrected |

Seen and left as they are: `GpioBm_Pin_t` is used by no consumer yet (G10);
`DeinitPin` has no `allow_debug_pins` (§10); `GPIO_HAL_Init` recomputes
`check()` for each pin it applies, which is redundant but correct.

### 12.2 Audit of the pre-F1 code (G1–G15)

The audit of the pre-F1 code found these. Their state at this commit:

| ID | Finding | State |
| :--- | :--- | :--- |
| G1 | The configuration registers have one copy for both cores; unprotected RMW from CM7 (PB13, ETH) and CM4 (PB1, ADC) on `GPIOB_MODER` in `prj_04` can put PB13 back to analog, with the link still up and TX dead | **Closed in the driver** (section + hook, T15). **Open in `prj_04`**, which does not install the hook (§13.1) |
| G2 | Wrong dual-core note in `gpio_bm.h` about `RCC_AHB4ENR` | Closed (`a2096d2`) |
| G3 | `pull = 3` programmed `PUPDR = 11` and `speed = 4` became LOW, both returning `true` | Closed (`b52ffec`), T1 |
| G4 | Misaligned port pointers (`GPIOA + 4`) accepted | Closed (`a204b8a`), T1 |
| G5 | No output or input API | Closed: `ConfigOutput/Input/Pin` in all tiers, T2–T4 |
| G6 | No EXTI; `HAL_GPIO_Init` rewrites `EXTICR` blindly and `HAL_GPIO_DeInit` clears the shared `RTSR1/FTSR1` | **Open (F2, §13.2).** The HAL tier refuses EXTI modes and never calls `HAL_GPIO_DeInit` |
| G7 | `HAL_GPIO_Init` analog + `PULLUP` does not write `PUPDR` | Closed in the HAL tier (refused); shown on raw HAL in T2 |
| G8 | No `LCKR`, deinit, `ReadConfig`, compensation cell | Closed except the compensation cell (F2) |
| G9 | Reconfiguring PA13/PA14 silently kills SWD | Closed: `ERR_DEBUG_PIN` in the new API; the legacy calls still accept them |
| G10 | Five pin types across consumers; `board_def.h` uses masks, `gpio_bm` indices | `GpioBm_Pin_t`, `GPIO_BM_PIN_MASK/INDEX` exist; no consumer uses `GpioBm_Pin_t` yet, and the existing types are untouched |
| G11 | Target without tier suffix | Closed in F0: `_gpio_bm` + alias |
| G12 | Output, OD, pulls, input, lock and the race never exercised in hardware | Closed for F1 (§9); EXTI pending |
| G13 | Writers outside `gpio_bm` on shared ports: `HAL_GPIO_Init` in the `eth_hal`/`uart_hal` MSPs, hand RMW in `prj_04` | `GPIO_HAL_Init` and the section exist (§3.3, T15 `g`/`h`); **the migration in `prj_04` is open** (§13.1) |
| G14 | `LED_PanicRed` writes `MODER` from a fault handler without the lock | Accepted exception: the fault path is terminal and cannot wait |
| G15 | `bool` cannot tell "lock busy" from "bad parameter" | Closed: `GpioBm_Status_t` |
| — | `LockPins` refused to add pins to a locked port (`ERR_LOCKED`) although the silicon accepts them | Closed (`bbb7f40`), T16 |

### 12.3 Re-audit 1 (R1–R4, 2026-09-30, at `1a635d8`)

A review of everything after the A1–A7 audit (`eab46d1` … `1a635d8`). The
fix of `0cd803f` and the A4 fix are correct. Every figure of §9.1 and §9.2
matches its `0cd803f` capture, and the two runs are independent (their analog
measurements differ). Corrections: `REAUDIT_1_INSTRUCTIONS.md`.

| ID | Finding | State |
| :--- | :--- | :--- |
| R1 | `GPIO_BM_ReadConfig`/`GPIO_LL_ReadConfig` return `GPIO_BM_OK` on a port unclocked in both banks, with the all-ones read turned into fields: analog, OD, VERY_HIGH, `pull = 3` (reserved), AF15, level 1. An `OK` that does not describe the hardware (C12) | **Open**: `GPIO_BM_ERR_NO_CLOCK` and `GPIO_BM_IsPortClocked`; case C of `T16 A1` |
| R2 | T15 is deterministic per image: every run of one image gives the same race counts (`c_raw_both` 315 897 ×4 at `780dddd`/`2b9bf21`, 320 906 ×2 at `0cd803f`; ISR 1 589 in 12 821 both times). Each protected phase has been tested on one interleaving, and the second run adds no race sample | **Open**: RNG seed per run, jitter outside the section |
| R3 | The mutation captures do not record their patch; only the file name and `6d1ef62` say what was mutated (C9) | **Open**: `gpio_hw_check.py` writes the `git diff` of a `-dirty` Rev into the capture |
| R4 | The measured silicon fact (an unclocked port reads `0xFFFFFFFF`) is missing from §6 | **Open**: one row in §6 |

---

## 13. Open work

### 13.1 F1b in `prj_04` (not done: outside the scope of this delivery)

`prj_04` has its own F1a selftest (preset `selftest-gpio`,
`CM4/CM7/App/Src/gpio_selftest.c`, `tools/gpio_hw_check.py`, protocol table in
`Common/Inc/gpio_bench_protocol.h`, captures `docs/gpio_f1a_*`). Its last
complete run (`gpio_f1a_selftest_ededa84.txt`) passed every item except the T9
gate on PF14, measured with 10 edges. The 40-edge interleaved procedure (errata
E9, used by the `prj_01` bench) was never applied there.

To close G1/G13 in `prj_04`, every writer that runs after CPU1 releases CPU2 on
a port both cores use must go through the driver or the section:

| Core | Writer | Port / pins | Change |
| :--- | :--- | :--- | :--- |
| CM7 | `eth_hal.c` MSP (`net_task`) | GPIOA, **GPIOB PB13**, GPIOC, GPIOG | `GPIO_HAL_Init` (a commit of its own, coordinated with `08_eth` E1/E2) |
| CM7 | `uart_hal.c` MSP (`App_Log_Init`) | GPIOD PD8/PD9 | `GPIO_HAL_Init` |
| CM4 | `adc_acquisition.c` `GPIO_Analog_Init` | **GPIOB PB1** | `GPIO_BM_ConfigAnalog(BOARD_ADC_PORT, GPIO_BM_PIN_INDEX(BOARD_ADC_PIN))` |
| CM4 | `main.c` (`selftest-uart`), `dma_selftest.c` | GPIOD PD5/PD6 | `gpio_bm` calls |
| CM7 | `LED_Init` (before the release), `LED_PanicRed` | GPIOB, GPIOE | None (safe by contract / G14) |
| CM7 | H5c of `10_hsem` (EXTI line 15, `selftest-hsem`) | EXTI | None (outside this driver) |

It also needs `Shared_GpioLockInstall()` on both cores, as in `prj_01` (§3),
and the production regression repeated with `mX_gpio_lock_fail_count = 0`.

### 13.2 F2: EXTI 0–15 and the compensation cell (not implemented)

The contract the plan fixed, to be taken up when a consumer needs it:

```c
GpioBm_Status_t GPIO_BM_ExtiConfig(GPIO_TypeDef *port, uint32_t pin_idx, uint32_t trigger,
                                   bool interrupt, bool event);        /* calling core's masks only */
GpioBm_Status_t GPIO_BM_ExtiDisable(GPIO_TypeDef *port, uint32_t pin_idx);
uint32_t        GPIO_BM_ExtiTakePending(uint32_t line_mask);          /* this core's PR1, then DSB */
GpioBm_Status_t GPIO_BM_ExtiSoftwareTrigger(uint32_t pin_idx);
IRQn_Type       GPIO_BM_ExtiGetIRQn(uint32_t pin_idx);
GpioBm_Status_t GPIO_BM_EnableCompensationCell(uint32_t max_cycles);  /* needs CSI; never writes RCC_CR or HSLV */
```

Rules:
- `EXTI_D2` with `CORE_CM4`, `EXTI_D1` otherwise.
- `RTSR1`, `FTSR1`, `SWIER1` and `SYSCFG_EXTICR` are shared, so they are
  written inside the section.
- A line routed to another port that any core has unmasked returns
  `ERR_CONFLICT`, without writing.
- The pending bit is cleared before unmasking.
- Deinit clears only this core's masks, and touches `RTSR1`/`FTSR1`/`EXTICR`
  only when the other core's masks are 0.
- No handler in the driver.

Planned tests: T11–T14 on PE3 (CM7) and PE4 (CM4), which are already wired
(§8): pulse counts, per-core routing, latency, and conflict/pending/deinit.
T17 covers the compensation cell.

---

## 14. Changing this driver

Catalog criteria every change is audited against:

| # | Criterion |
| :--- | :--- |
| C1 | No pins or instances: no port, pin, HSEM ID or time limit of a project; no `board_def.h`, `shared_memory.h` or `hsem_*.h`. Only silicon facts (stride 0x400, GPIOA..GPIOK, `GPIOxEN` = port index, PA13/PA14 = SWD) |
| C2 | Frozen API: the legacy signatures and constants and the F1 API never change; only additions. A behaviour change only where the old one was wrong, still accepting every existing call |
| C3 | Tier suffix on every file; no numeric prefix in any `#include` |
| C4 | Targets: `_gpio_bm` with its source; `stm32h7_driver_gpio` an alias without sources; `_ll` and `_hal` link `_bm` |
| C5 | Errata applied in the driver when possible, otherwise listed here (§6) |
| C6 | Validate before the first access, including the port clock |
| C7 | Minimal state: `gpio_bm` ≤ 8 B of `.bss` and `.data = 0`; none in LL and HAL |
| C8 | `gpio_bm` depends on CMSIS only and works before the clock tree, the MPU, `HAL_Init()` and the HSEM clock |
| C9 | Evidence: build matrix, captures with `FW_GIT_REV`, sizes from the linked `.map` |
| C10 | Every RMW of a shared configuration register inside the section |
| C11 | Glitch-free write order (§4.3) |
| C12 | Honesty: never `OK` for a write the hardware ignored; read-back after every write |
| C13 | Every wait bounded by a limit the caller gives |
| C14 | The driver defines no project symbol (`EXTIx_IRQHandler`, `HAL_GPIO_EXTI_Callback`) |

Before changing it:

1. **Signatures and existing struct fields are frozen**
   (`01_shared_drivers/README.md` rule 2). Add new functions, or new fields at
   the end of a struct. Check with
   `git diff main -- '01_shared_drivers/00_gpio/*.h'`: the diff may only add
   lines or change comments.
2. **Keep the three tiers on the single validation of `gpio_bm`** and give them
   the same status in every case. Extend T1/T2 for every new path.
3. **`gpio_bm` stays CMSIS-only** and usable before the clock tree, the MPU and
   `HAL_Init()`.
4. **Every test needs a negative control that prints its value**, and an oracle
   that is not the driver.
5. **Captures are the output of one run** from a clean commit, and are never
   edited.
6. **Before merging:** run `.\tools\build_all.ps1 -Clean` (8/8), two complete
   `gpio_hw_check.py` runs on the bench, and the production warm resets.

---

## 15. History

| Milestone | Commits |
| :--- | :--- |
| Port-clock helper, `ConfigAlternate`, `ConfigAnalog` | `784febb`, `dcd9230`, `be31f3c` |
| Plan (revisions 1 and 2), F0 bench; audit closed F0 at `51a29b7` | `5f97e2e` … `51a29b7` |
| F1a: `gpio_bm` core, section and hook, `LCKR`, `ReadConfig` (prj_04 selftest) | `a94c89c` … `b52ffec` |
| LL and HAL tiers, shared validation | `6c864c8`, `de138ba`, `b9c1a76` |
| `prj_01`: hook on both cores; limit sized from B1 (50 µs) | `5c6f849`, `35b5e4e` |
| `LockPins` adds pins to a locked port (silicon finding) | `bbb7f40` |
| `prj_01` selftest, host bench, evidence | `e3feb02`, `780dddd`, `d11fb8d`, `2b9bf21`, `778fcbb` |
| README; auditor's errata E9–E11 committed; process documents removed | `7d421d1`, `33f4afa`, `cc1ee88` |
| Code audit of F1 and the LL/HAL tiers at `cc1ee88`; fixes A1, A4, A6, A7 | `eab46d1` |
| Selftest steps for the audit: TB A4, T16 A1 | `8b8919a` |
| Bench: the A1 premise refuted (unclocked port reads all ones); A1/A5 corrected; runs, regression, mutations | `cdbb914`, `0cd803f`, `6d1ef62` |

The implementation plan, the implementer's instructions, the F0 correction and
F1a instructions, the implementation report and the audit record were removed
once what a user of the driver needs was folded into this README. The audit
closed F0 at `51a29b7`. F1 and the LL/HAL tiers had a code audit at `cc1ee88`
(§12.1); its fixes passed the bench at `0cd803f`. The documents stay in git
history:

```bash
git show 33f4afa:01_shared_drivers/00_gpio/IMPLEMENTATION_PLAN.md
git show 33f4afa:01_shared_drivers/00_gpio/AUDIT.md
git show 33f4afa:01_shared_drivers/00_gpio/IMPLEMENTATION_REPORT.md
git show 33f4afa:01_shared_drivers/00_gpio/IMPLEMENTER_INSTRUCTIONS.md
git show 33f4afa:01_shared_drivers/00_gpio/F1A_INSTRUCTIONS.md
git show 33f4afa:01_shared_drivers/00_gpio/F0_CORRECTION_INSTRUCTIONS.md   # also _2, _3, _4
git show 5f97e2e:01_shared_drivers/00_gpio/IMPLEMENTATION_PLAN.md          # revision 1: full LL/HAL contracts
```
