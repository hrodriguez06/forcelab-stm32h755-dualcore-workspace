/*
******************************************************************************
* @file      stm32h755zi_cm7.ld
* @brief     Linker script for STM32H755ZI - CPU1 (Cortex-M7), Flash Bank 1.
*
*            Memory partition used by this project (see README.md):
*              M7  code   : FLASH bank 1   0x08000000  1 MB
*              M7  data   : DTCM           0x20000000  128 KB   (no DMA1/DMA2!)
*              M7  fast fn: ITCM           0x00000000  64 KB
*              M7  bulk   : AXI SRAM (D1)  0x24000000  512 KB
*              M7  DMA    : SRAM1 (D2)     0x30000000  128 KB   non-cacheable
*              ETH        : SRAM3 (D2)     0x30040000  32 KB    non-cacheable
*              BDMA/D3    : SRAM4 (D3)     0x38000000  32 KB    non-cacheable
*              Shared M7/M4:SRAM4 (D3)     0x38008000  32 KB    non-cacheable
******************************************************************************
*/

ENTRY(Reset_Handler)

/* ---- Stack / heap for CPU1 -------------------------------------------- */
_estack        = ORIGIN(DTCMRAM) + LENGTH(DTCMRAM);   /* top of DTCM */
_Min_Heap_Size = 0x2000;      /*  8 KB */
_Min_Stack_Size = 0x4000;     /* 16 KB */

MEMORY
{
  ITCMRAM   (xrw) : ORIGIN = 0x00000000, LENGTH = 64K
  FLASH     (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K   /* Bank 1 - CPU1 */
  DTCMRAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
  RAM_D1    (xrw) : ORIGIN = 0x24000000, LENGTH = 512K    /* AXI SRAM      */
  RAM_D2_M7 (xrw) : ORIGIN = 0x30000000, LENGTH = 128K    /* SRAM1 - DMA   */
  RAM_D2_ETH(xrw) : ORIGIN = 0x30040000, LENGTH = 32K     /* SRAM3 - ETH   */
  RAM_D3    (xrw) : ORIGIN = 0x38000000, LENGTH = 32K     /* SRAM4 - BDMA  */
  RAM_SHARED(xrw) : ORIGIN = 0x38008000, LENGTH = 32K     /* SRAM4 - IPC   */
}

ASSERT(ORIGIN(RAM_SHARED) == 0x38008000, "RAM_SHARED ORIGIN must match SHARED_RAM_BASE (0x38008000) in shared_memory.h")
ASSERT(LENGTH(RAM_SHARED) == 32K, "RAM_SHARED LENGTH must match SHARED_RAM_SIZE (32K) in shared_memory.h")

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    KEEP(*(.init))
    KEEP(*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM       : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } >FLASH

  .preinit_array : { PROVIDE_HIDDEN(__preinit_array_start = .); KEEP(*(.preinit_array*)) PROVIDE_HIDDEN(__preinit_array_end = .); } >FLASH
  .init_array    : { PROVIDE_HIDDEN(__init_array_start = .);    KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array*)) PROVIDE_HIDDEN(__init_array_end = .); } >FLASH
  .fini_array    : { PROVIDE_HIDDEN(__fini_array_start = .);    KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array*)) PROVIDE_HIDDEN(__fini_array_end = .); } >FLASH

  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    *(.RamFunc)
    *(.RamFunc*)
    . = ALIGN(4);
    _edata = .;
  } >DTCMRAM AT> FLASH

  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >DTCMRAM

  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE(end = .);
    PROVIDE(_end = .);
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >DTCMRAM

  _si_itcm = LOADADDR(.itcm);
  .itcm :
  {
    . = ALIGN(8);
    _s_itcm = .;
    *(.itcm_text)
    *(.itcm_text*)
    . = ALIGN(8);
    _e_itcm = .;
  } >ITCMRAM AT> FLASH

  .axi_ram (NOLOAD) :
  {
    . = ALIGN(32);
    _s_axi_ram = .;
    *(.axi_ram)
    *(.axi_ram*)
    . = ALIGN(32);
    _e_axi_ram = .;
  } >RAM_D1

  .dma_buf (NOLOAD) :
  {
    . = ALIGN(32);
    _s_dma_buf = .;
    *(.dma_buf)
    *(.dma_buf*)
    . = ALIGN(32);
    _e_dma_buf = .;
  } >RAM_D2_M7

  .eth_ram (NOLOAD) :
  {
    . = ALIGN(32);
    _s_eth_ram = .;
    *(.RxDescripSection)
    *(.TxDescripSection)
    *(.RxArraySection)
    *(.eth_ram)
    *(.eth_ram*)
    . = ALIGN(32);
    _e_eth_ram = .;
  } >RAM_D2_ETH

  .d3_ram (NOLOAD) :
  {
    . = ALIGN(32);
    _s_d3_ram = .;
    *(.d3_ram)
    *(.d3_ram*)
    . = ALIGN(32);
    _e_d3_ram = .;
  } >RAM_D3

  .shared_ram (NOLOAD) :
  {
    . = ALIGN(32);
    _s_shared_ram = .;
    KEEP(*(.shared_ram))
    KEEP(*(.shared_ram*))
    . = ALIGN(32);
    _e_shared_ram = .;
  } >RAM_SHARED

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
