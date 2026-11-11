/*
******************************************************************************
* @file      stm32h755zi_cm4.ld
* @brief     Linker script for STM32H755ZI - CPU2 (Cortex-M4), Flash Bank 2.
*
*            Memory partition used by this project (see README.md):
*              M4  code   : FLASH bank 2   0x08100000  1 MB
*              M4  data   : SRAM2 (D2)     0x30020000  128 KB
*              BDMA/D3    : SRAM4 (D3)     0x38000000  32 KB    non-cacheable
*              Shared M7/M4:SRAM4 (D3)     0x38008000  32 KB    non-cacheable
******************************************************************************
*/

ENTRY(Reset_Handler)

_estack         = ORIGIN(RAM_D2_M4) + LENGTH(RAM_D2_M4);
_Min_Heap_Size  = 0x1000;     /*  4 KB */
_Min_Stack_Size = 0x2000;     /*  8 KB */

MEMORY
{
  FLASH     (rx)  : ORIGIN = 0x08100000, LENGTH = 1024K   /* Bank 2 - CPU2 */
  RAM_D2_M4 (xrw) : ORIGIN = 0x30020000, LENGTH = 128K    /* SRAM2         */
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

  .rodata : { . = ALIGN(4); *(.rodata) *(.rodata*) . = ALIGN(4); } >FLASH

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
  } >RAM_D2_M4 AT> FLASH

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
  } >RAM_D2_M4

  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE(end = .);
    PROVIDE(_end = .);
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM_D2_M4

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
