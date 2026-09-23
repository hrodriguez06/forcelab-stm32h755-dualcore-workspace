/**
  ******************************************************************************
  * @file    mpu_config.c
  * @brief   MPU setup for STM32H755ZI CPU1 (Cortex-M7).
  *
  *          Marks every DMA-visible region declared in stm32h755zi_cm7.ld as
  *          Normal, NON-CACHEABLE, so DMA and the M7 see the same bytes without
  *          manual clean/invalidate.
  *
  *          Call order in main():
  *              MPU_Config();          <-- BEFORE SCB_EnableDCache()
  *              SCB_EnableICache();
  *              SCB_EnableDCache();
  *              HAL_Init();
  *
  *          Region map (ARMv7-M MPU: base must be size-aligned, size = 2^n,
  *          higher region number wins on overlap):
  *
  *            R0  0x00000000  4 GB    background guard   no access, XN
  *            R1  0x00000000  64 KB   ITCM   .itcm_text  exec
  *            R2  0x08000000  2 MB    FLASH  bank 1 + 2  exec, cacheable WB
  *            R3  0x20000000  128 KB  DTCM   stack/heap  XN, cacheable
  *            R4  0x24000000  512 KB  AXI SRAM .axi_ram  exec, cacheable WB
  *            R5  0x30000000  128 KB  SRAM1  .dma_buf    XN, non-cacheable
  *            R6  0x30040000  32 KB   SRAM3  .eth_ram    XN, non-cacheable
  *            R7  0x38000000  64 KB   SRAM4  .d3_ram + .shared_ram  XN, NC
  *            R8  0x40000000  512 MB  peripherals        XN, Device
  *
  *            R9..R15 are free for project-specific additions - external
  *            memory windows (FMC / QUADSPI, see docs/peripherals/fmc_quadspi),
  *            a tighter guard around an unused bank, and so on.
  *
  * @warning R0 denies EVERYTHING, which is the ES0445 2.1.2 workaround: it
  *          stops a speculative PLD from line-filling *unmapped* memory. It is
  *          only safe because R1..R8 map back in every memory the firmware
  *          actually uses. Delete any one of them and the CPU locks up the
  *          instant HAL_MPU_Enable() runs: the next instruction fetch from
  *          FLASH raises MemManage (IACCVIOL) and the exception stack push
  *          into DTCM raises MSTKERR, and neither the handler nor its stack is
  *          reachable, so it escalates straight to HardFault lockup. The board
  *          goes silent before the clock tree is even configured, which looks
  *          like a dead flash image rather than an MPU problem.
  *          Symptom check: MPU_CTRL = 0x05, SCB_CFSR = 0x11, SCB_HFSR bit 30.
  *
  * @note    The Private Peripheral Bus at 0xE0000000 (SCB, NVIC, SysTick, DWT,
  *          ITM) is exempt from the MPU by architecture and needs no region.
  *          Peripherals at 0x40000000..0x5FFFFFFF are NOT exempt - that is
  *          what R8 is for, and without it every RCC and GPIO access faults.
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

    /* R0: background guard over the whole map. Blocks speculative PLD from
       line-filling unmapped device memory. ES0445 2.1.2.
       Everything the firmware really uses is mapped back in by R1..R8 - see
       the warning in the file header before removing any of them.           */
    MPU_Region(MPU_REGION_NUMBER0, 0x00000000, MPU_REGION_SIZE_4GB,
               MPU_TEX_LEVEL0, MPU_ACCESS_NOT_CACHEABLE,
               MPU_ACCESS_NOT_BUFFERABLE, MPU_ACCESS_NOT_SHAREABLE,
               MPU_REGION_NO_ACCESS, MPU_INSTRUCTION_ACCESS_DISABLE);

    /* R1: ITCM - time-critical code (.itcm_text). Must stay executable.
       TCM accesses bypass the L1 caches, so the cache attributes are nominal;
       the access permission is what matters.                                */
    MPU_Region(MPU_REGION_NUMBER1, 0x00000000, MPU_REGION_SIZE_64KB,
               MPU_TEX_LEVEL1, MPU_ACCESS_NOT_CACHEABLE,
               MPU_ACCESS_NOT_BUFFERABLE, MPU_ACCESS_NOT_SHAREABLE,
               MPU_REGION_FULL_ACCESS, MPU_INSTRUCTION_ACCESS_ENABLE);

    /* R2: FLASH bank 1 + bank 2 - the code this core executes. Cacheable
       write-back; the ART accelerator sits in front of it.                  */
    MPU_Region(MPU_REGION_NUMBER2, 0x08000000, MPU_REGION_SIZE_2MB,
               MPU_TEX_LEVEL1, MPU_ACCESS_CACHEABLE,
               MPU_ACCESS_BUFFERABLE, MPU_ACCESS_NOT_SHAREABLE,
               MPU_REGION_FULL_ACCESS, MPU_INSTRUCTION_ACCESS_ENABLE);

    /* R3: DTCM - .data, .bss, the main stack and the RTOS heap.
       Execute-never: nothing legitimately runs from here. Unreachable by
       DMA1/DMA2 by construction, so no DMA buffer may live here.            */
    MPU_Region(MPU_REGION_NUMBER3, 0x20000000, MPU_REGION_SIZE_128KB,
               MPU_TEX_LEVEL1, MPU_ACCESS_CACHEABLE,
               MPU_ACCESS_BUFFERABLE, MPU_ACCESS_NOT_SHAREABLE,
               MPU_REGION_FULL_ACCESS, MPU_INSTRUCTION_ACCESS_DISABLE);

    /* R4: AXI SRAM - bulk buffers (.axi_ram). Cacheable write-back.
       Buffers here STILL need SCB_Clean/InvalidateDCache_by_Addr() + __DMB()
       around every DMA transfer. ES0445 2.1.5.
       Never mark this write-through: ES0445 2.1.1.                          */
    MPU_Region(MPU_REGION_NUMBER4, 0x24000000, MPU_REGION_SIZE_512KB,
               MPU_TEX_LEVEL1, MPU_ACCESS_CACHEABLE,
               MPU_ACCESS_BUFFERABLE, MPU_ACCESS_NOT_SHAREABLE,
               MPU_REGION_FULL_ACCESS, MPU_INSTRUCTION_ACCESS_ENABLE);

    /* R5: SRAM1 - DMA1 / DMA2 buffers (.dma_buf). Normal, non-cacheable.    */
    MPU_Region(MPU_REGION_NUMBER5, 0x30000000, MPU_REGION_SIZE_128KB,
               MPU_TEX_LEVEL1, MPU_ACCESS_NOT_CACHEABLE,
               MPU_ACCESS_NOT_BUFFERABLE, MPU_ACCESS_SHAREABLE,
               MPU_REGION_FULL_ACCESS, MPU_INSTRUCTION_ACCESS_DISABLE);

    /* R6: SRAM3 - Ethernet descriptors and buffers (.eth_ram). This is the
       region the ETH DMA fetches its descriptors from: it must stay Normal,
       non-cacheable and shareable, or the D-Cache and the ETH DMA disagree on
       descriptor ownership and reception stalls after a few frames.         */
    MPU_Region(MPU_REGION_NUMBER6, 0x30040000, MPU_REGION_SIZE_32KB,
               MPU_TEX_LEVEL1, MPU_ACCESS_NOT_CACHEABLE,
               MPU_ACCESS_NOT_BUFFERABLE, MPU_ACCESS_SHAREABLE,
               MPU_REGION_FULL_ACCESS, MPU_INSTRUCTION_ACCESS_DISABLE);

    /* R7: SRAM4 - BDMA buffers (.d3_ram) + M7/M4 shared block (.shared_ram).
       Non-cacheable is MANDATORY here: the M4 has no cache and would never
       see data still sitting in the M7 write buffer.                        */
    MPU_Region(MPU_REGION_NUMBER7, 0x38000000, MPU_REGION_SIZE_64KB,
               MPU_TEX_LEVEL1, MPU_ACCESS_NOT_CACHEABLE,
               MPU_ACCESS_NOT_BUFFERABLE, MPU_ACCESS_SHAREABLE,
               MPU_REGION_FULL_ACCESS, MPU_INSTRUCTION_ACCESS_DISABLE);

    /* R8: peripherals, 0x40000000..0x5FFFFFFF - every APB and AHB register
       block, including RCC, SYSCFG, GPIO, the DMA controllers and the
       Ethernet MAC. Device memory (TEX0/C0/B1), shareable, execute-never:
       registers must never be cached, nor speculatively executed from.      */
    MPU_Region(MPU_REGION_NUMBER8, 0x40000000, MPU_REGION_SIZE_512MB,
               MPU_TEX_LEVEL0, MPU_ACCESS_NOT_CACHEABLE,
               MPU_ACCESS_BUFFERABLE, MPU_ACCESS_SHAREABLE,
               MPU_REGION_FULL_ACCESS, MPU_INSTRUCTION_ACCESS_DISABLE);

    /* R9: System Memory / Information block, 0x1FF00000..0x1FF1FFFF (128 KB)
       Contains factory calibration values (VREFINT_CAL, TS_CAL1, TS_CAL2). */
    MPU_Region(MPU_REGION_NUMBER9, 0x1FF00000, MPU_REGION_SIZE_128KB,
               MPU_TEX_LEVEL0, MPU_ACCESS_NOT_CACHEABLE,
               MPU_ACCESS_NOT_BUFFERABLE, MPU_ACCESS_SHAREABLE,
               MPU_REGION_FULL_ACCESS, MPU_INSTRUCTION_ACCESS_DISABLE);

    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

