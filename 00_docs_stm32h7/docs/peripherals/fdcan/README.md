# FDCAN (Flexible Data-rate Controller Area Network) - STM32H755

## 1. Features
- Supports classic **CAN 2.0B** and **CAN FD** (up to 64 data bytes per frame, baud rates up to 5-8 Mbps in data phase).
- **Instances**: FDCAN1, FDCAN2 (both in D2 domain).
- Dedicated Message RAM (10 KB shared between FDCAN1 and FDCAN2 at `0x4000AC00`).

---

## 2. Clock Source & Bit Timing
- Kernel Clock Source: Usually sourced from `hse_ck`, `pll1_q_ck`, or `pll2_q_ck` via `RCC->D2CCIP1R`.
- Two sets of bit timing parameters:
  1. **Nominal Bit Timing**: For arbitration phase (e.g. 500 kbps).
  2. **Data Bit Timing**: For payload data phase (e.g. 2 Mbps or 5 Mbps).

---

## 3. Basic HAL Initialization Pattern
```c
FDCAN_HandleTypeDef hfdcan1;

hfdcan1.Instance                  = FDCAN1;
hfdcan1.Init.FrameFormat          = FDCAN_FRAME_FD_BRS; /* Bit Rate Switching */
hfdcan1.Init.Mode                 = FDCAN_MODE_NORMAL;
hfdcan1.Init.AutoRetransmission   = ENABLE;
hfdcan1.Init.TransmitPause        = DISABLE;
hfdcan1.Init.ProtocolException    = DISABLE;

/* Nominal Timings (500 kbps with 80 MHz clock) */
hfdcan1.Init.NominalPrescaler     = 1;
hfdcan1.Init.NominalSyncJumpWidth = 16;
hfdcan1.Init.NominalTimeSeg1      = 127;
hfdcan1.Init.NominalTimeSeg2      = 32;

/* Data Timings (2 Mbps with 80 MHz clock) */
hfdcan1.Init.DataPrescaler        = 1;
hfdcan1.Init.DataSyncJumpWidth    = 4;
hfdcan1.Init.DataTimeSeg1         = 31;
hfdcan1.Init.DataTimeSeg2         = 8;

HAL_FDCAN_Init(&hfdcan1);
```

---

## 4. Message RAM & Filters
Before starting FDCAN, at least one standard/extended ID filter must be configured, or the global filter must be set to accept non-matching frames:
```c
HAL_FDCAN_ConfigGlobalFilter(&hfdcan1, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_FILTER_REMOTE, FDCAN_FILTER_REMOTE);
HAL_FDCAN_Start(&hfdcan1);
```
