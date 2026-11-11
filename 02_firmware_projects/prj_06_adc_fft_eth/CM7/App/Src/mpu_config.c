/**
  ******************************************************************************
  * @file    mpu_config.c
  * @brief   MPU setup for STM32H755ZI CPU1 (Cortex-M7).
  *
  *          Region map:
  *            R0  0x00000000  4 GB    background guard   no access, XN (ES0445 2.1.2)
  *            R1  0x00000000  64 KB   ITCM   .itcm_text  exec
  *            R2  0x08000000  2 MB    FLASH  bank 1 + 2  exec, cacheable WB
  *            R3  0x20000000  128 KB  DTCM   stack/heap  XN, cacheable
  *            R4  0x24000000  512 KB  AXI SRAM .axi_ram  exec, cacheable WB
  *            R5  0x30000000  128 KB  SRAM1  .dma_buf    XN, non-cacheable
  *            R6  0x30040000  32 KB   SRAM3  .eth_ram    XN, non-cacheable
  *            R7  0x38000000  64 KB   SRAM4  .d3_ram + .shared_ram  XN, NC
  *            R8  0x40000000  512 MB  peripherals        XN, Device
  *            R9  0x1FF00000  128 KB  System ROM (cal)   XN, RO, NC
  *            R10 0x38800000  4 KB    Backup SRAM        XN, NC
  ******************************************************************************
  */

#include "main.h"
#include "mpu_config.h"

static void MPU_Region(uint8_t number, uint32_t base, uint8_t size,
                       uint8_t tex, uint8_t cacheable, uint8_t bufferable,
                       uint8_t shareable, uint8_t access, uint8_t xn)
{
    MPU_Region_InitTypeDef r = {0};

    r.Enable             = MPU_REGION_ENABLE;
    r.Number             = number;
    r.BaseAddress        = base;
    r.Size               = size;
    r.SubRegionDisable   = 0x00;
    r.TypeExtField       = tex;
    r.AccessPermission   = access;
    r.DisableExec        = xn;
    r.IsShareable        = shareable;
    r.IsCacheable        = cacheable;
    r.IsBufferable       = bufferable;

    HAL_MPU_ConfigRegion(&r);
}

void MPU_Config(void)
{
    HAL_MPU_Disable();

    MPU_Region(MPU_REGION_NUMBER0, 0x00000000, MPU_REGION_SIZE_4GB,
               MPU_TEX_LEVEL0, MPU_ACCESS_NOT_CACHEABLE,
               MPU_ACCESS_NOT_BUFFERABLE, MPU_ACCESS_NOT_SHAREABLE,
               MPU_REGION_NO_ACCESS, MPU_INSTRUCTION_ACCESS_DISABLE);

    MPU_Region(MPU_REGION_NUMBER1, 0x00000000, MPU_REGION_SIZE_64KB,
               MPU_TEX_LEVEL1, MPU_ACCESS_NOT_CACHEABLE,
               MPU_ACCESS_NOT_BUFFERABLE, MPU_ACCESS_NOT_SHAREABLE,
               MPU_REGION_FULL_ACCESS, MPU_INSTRUCTION_ACCESS_ENABLE);

    MPU_Region(MPU_REGION_NUMBER2, 0x08000000, MPU_REGION_SIZE_2MB,
               MPU_TEX_LEVEL1, MPU_ACCESS_CACHEABLE,
               MPU_ACCESS_BUFFERABLE, MPU_ACCESS_NOT_SHAREABLE,
               MPU_REGION_FULL_ACCESS, MPU_INSTRUCTION_ACCESS_ENABLE);

    MPU_Region(MPU_REGION_NUMBER3, 0x20000000, MPU_REGION_SIZE_128KB,
               MPU_TEX_LEVEL1, MPU_ACCESS_CACHEABLE,
               MPU_ACCESS_BUFFERABLE, MPU_ACCESS_NOT_SHAREABLE,
               MPU_REGION_FULL_ACCESS, MPU_INSTRUCTION_ACCESS_DISABLE);

    MPU_Region(MPU_REGION_NUMBER4, 0x24000000, MPU_REGION_SIZE_512KB,
               MPU_TEX_LEVEL1, MPU_ACCESS_CACHEABLE,
               MPU_ACCESS_BUFFERABLE, MPU_ACCESS_NOT_SHAREABLE,
               MPU_REGION_FULL_ACCESS, MPU_INSTRUCTION_ACCESS_ENABLE);

    MPU_Region(MPU_REGION_NUMBER5, 0x30000000, MPU_REGION_SIZE_128KB,
               MPU_TEX_LEVEL1, MPU_ACCESS_NOT_CACHEABLE,
               MPU_ACCESS_NOT_BUFFERABLE, MPU_ACCESS_SHAREABLE,
               MPU_REGION_FULL_ACCESS, MPU_INSTRUCTION_ACCESS_DISABLE);

    MPU_Region(MPU_REGION_NUMBER6, 0x30040000, MPU_REGION_SIZE_32KB,
               MPU_TEX_LEVEL1, MPU_ACCESS_NOT_CACHEABLE,
               MPU_ACCESS_NOT_BUFFERABLE, MPU_ACCESS_SHAREABLE,
               MPU_REGION_FULL_ACCESS, MPU_INSTRUCTION_ACCESS_DISABLE);

    MPU_Region(MPU_REGION_NUMBER7, 0x38000000, MPU_REGION_SIZE_64KB,
               MPU_TEX_LEVEL1, MPU_ACCESS_NOT_CACHEABLE,
               MPU_ACCESS_NOT_BUFFERABLE, MPU_ACCESS_SHAREABLE,
               MPU_REGION_FULL_ACCESS, MPU_INSTRUCTION_ACCESS_DISABLE);

    MPU_Region(MPU_REGION_NUMBER8, 0x40000000, MPU_REGION_SIZE_512MB,
               MPU_TEX_LEVEL0, MPU_ACCESS_NOT_CACHEABLE,
               MPU_ACCESS_BUFFERABLE, MPU_ACCESS_SHAREABLE,
               MPU_REGION_FULL_ACCESS, MPU_INSTRUCTION_ACCESS_DISABLE);

    MPU_Region(MPU_REGION_NUMBER9, 0x1FF00000, MPU_REGION_SIZE_128KB,
               MPU_TEX_LEVEL0, MPU_ACCESS_NOT_CACHEABLE,
               MPU_ACCESS_NOT_BUFFERABLE, MPU_ACCESS_SHAREABLE,
               MPU_REGION_PRIV_RO_URO, MPU_INSTRUCTION_ACCESS_DISABLE);

    MPU_Region(MPU_REGION_NUMBER10, 0x38800000, MPU_REGION_SIZE_4KB,
               MPU_TEX_LEVEL1, MPU_ACCESS_NOT_CACHEABLE,
               MPU_ACCESS_NOT_BUFFERABLE, MPU_ACCESS_SHAREABLE,
               MPU_REGION_FULL_ACCESS, MPU_INSTRUCTION_ACCESS_DISABLE);

    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}
