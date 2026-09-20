# Ethernet MAC (10/100 Mbps) - STM32H755

The STM32H755 integrates a 10/100 Mbps Ethernet MAC with RMII / MII interface and hardware IEEE 1588 PTP support.

---

## 1. Nucleo-H755ZI-Q Hardware Connections
- **PHY Transceiver**: Microchip **LAN8742A** (Default PHY address = `0`).
- **Interface**: RMII (Reduced Media Independent Interface):
  - `ETH_REF_CLK`: PA1 (AF11)
  - `ETH_MDIO`: PA2 (AF11)
  - `ETH_MDC`: PC1 (AF11)
  - `ETH_CRS_DV`: PA7 (AF11)
  - `ETH_RXD0`: PC4 (AF11)
  - `ETH_RXD1`: PC5 (AF11)
  - `ETH_TX_EN`: PG11 (AF11)
  - `ETH_TXD0`: PG13 (AF11)
  - `ETH_TXD1`: PB13 (AF11)

---

## 2. Mandatory Memory & MPU Configuration (CRITICAL)

Ethernet DMA descriptors and packet buffers **MUST** be placed in **D2 Domain SRAM1/2/3** (`0x30000000` - `0x30047FFF`) and configured as **Device** or **Normal Non-Cacheable** in the MPU.

### A. Linker Placement (`STM32H755ZITX_FLASH.ld`)
```ld
.lwip_sec (NOLOAD) :
{
  . = ALIGN(4);
  *(.RxDecripSection)
  *(.TxDecripSection)
  *(.RxArraySection)
  *(.TxArraySection)
} >RAM_D2 /* 0x30000000 */
```

### B. MPU Configuration for Ethernet
```c
void MPU_Config_Ethernet(void)
{
    MPU_Region_InitTypeDef MPU_InitStruct = {0};

    HAL_MPU_Disable();

    /* Configure 16 KB of D2 SRAM at 0x30000000 as Non-Cacheable / Device */
    MPU_InitStruct.Enable           = MPU_REGION_ENABLE;
    MPU_InitStruct.Number           = MPU_REGION_NUMBER1;
    MPU_InitStruct.BaseAddress      = 0x30000000;
    MPU_InitStruct.Size             = MPU_REGION_SIZE_16KB;
    MPU_InitStruct.SubRegionDisable = 0x00;
    MPU_InitStruct.TypeExtField     = MPU_TEX_LEVEL0;
    MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
    MPU_InitStruct.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
    MPU_InitStruct.IsShareable      = MPU_ACCESS_SHAREABLE;
    MPU_InitStruct.IsCacheable      = MPU_ACCESS_NOT_CACHEABLE;
    MPU_InitStruct.IsBufferable     = MPU_ACCESS_BUFFERABLE;

    HAL_MPU_ConfigRegion(&MPU_InitStruct);

    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}
```

---

## 3. Clocking Requirement
- RMII Reference clock (`ETH_REF_CLK`, 50 MHz) can either be provided by the on-board oscillator/PHY or generated from STM32 PLL3/MCO. On Nucleo boards, the PHY generates 50 MHz fed into PA1.
