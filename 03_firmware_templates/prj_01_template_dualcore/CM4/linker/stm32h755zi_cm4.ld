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
*
*            The M4 has NO cache, so it never needs cache maintenance. It DOES
*            need the HSEM to guard everything it shares with the M7.
*
* @warning   BOOT_CM4 option byte must point at 0x08100000. Check with
*            STM32CubeProgrammer (UM2237): option bytes BCM4 / BOOT_CM4_ADD0.
* @warning   DTCM/ITCM and AXI SRAM belong to D1 and are NOT declared here:
*            the M4 can reach AXI SRAM through the bus matrix but it is slow
*            and it collides with the M7. Keep M4 data in SRAM2.
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

  /* ---- BDMA / D3 peripheral buffers (LPUART1, SPI6, I2C4) --------------- */
  .d3_ram (NOLOAD) :
  {
    . = ALIGN(32);
    _s_d3_ram = .;
    *(.d3_ram)
    *(.d3_ram*)
    . = ALIGN(32);
    _e_d3_ram = .;
  } >RAM_D3

  /* ---- Shared memory M7 <-> M4 -----------------------------------------
     MUST mirror the .shared_ram section of stm32h755zi_cm7.ld byte for byte.
     Any change to shared_memory.h requires rebuilding BOTH cores.        */
  .shared_ram (NOLOAD) :
  {
    . = ALIGN(32);
    _s_shared_ram = .;
    KEEP(*(.shared_ram))
    KEEP(*(.shared_ram*))
    . = ALIGN(32);
    _e_shared_ram = .;
  } >RAM_SHARED

  /* No /DISCARD/ rule here, on purpose. The one that used to sit at this spot
     - libc.a(*) libm.a(*) libgcc.a(*) - never removed a single byte, and the
     next person to notice its absence should not put it back.

     In a filename(sections) spec the filename is matched against the *input
     file*, and for a library that input file is the archive member
     (_udivmoddi4.o), never the archive holding it. A bare libgcc.a therefore
     matches nothing, and a leading wildcard does not help: the name being
     tested was never the archive's. Discarding by archive takes the
     archive:member form.

     Measured across the projects in this workspace: present, absent, and
     strengthened to a wildcard and hoisted above .text, the .bin and the whole
     ELF came out byte-identical every time.

     Writing it the way it clearly meant to be written is the worse outcome.
     *libgcc.a:*(*) does match, and the link then fails the moment the image
     needs a compiler helper - one 64-bit division pulls in __aeabi_uldivmod.
     Keeping parts of the C library out of an image is a link-line decision
     (-nostdlib, --specs=), not something a linker script should try to enforce
     behind the build system's back. */

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
