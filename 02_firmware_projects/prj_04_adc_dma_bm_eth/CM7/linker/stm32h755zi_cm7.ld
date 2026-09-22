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
*
*            SRAM2 (0x30020000, 128 KB) is reserved for CPU2 (M4) and is NOT
*            declared here on purpose.
*
* @warning   DTCM is NOT reachable by DMA1 / DMA2. Any buffer handed to those
*            controllers must be placed with __attribute__((section(".dma_buf")))
*            or .eth_ram / .d3_ram. See docs/00_core_and_system/memory_and_domains.md
* @warning   The non-cacheable regions below only behave as such once the MPU is
*            configured. See mpu_config.c in this folder.
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

SECTIONS
{
  /* ---- Vector table ---------------------------------------------------- */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  /* ---- Code ------------------------------------------------------------ */
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

  /* ---- Initialised data -> DTCM (fastest, zero wait state) ------------- */
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

  /* ---- Time-critical functions in ITCM ---------------------------------
     Usage:  void __attribute__((section(".itcm_text"))) fast_isr(void) {...}
     Copied from Flash by the startup code (see README.md).              */
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

  /* ---- Bulk buffers in AXI SRAM (D1, cacheable) -------------------------
     Usage:  uint8_t img[N] __attribute__((section(".axi_ram")));
     Reachable by MDMA and by DMA1/DMA2 through the bus matrix.
     CACHEABLE: clean/invalidate + __DMB() still required (ES0445 2.1.5). */
  .axi_ram (NOLOAD) :
  {
    . = ALIGN(32);
    _s_axi_ram = .;
    *(.axi_ram)
    *(.axi_ram*)
    . = ALIGN(32);
    _e_axi_ram = .;
  } >RAM_D1

  /* ---- DMA1 / DMA2 buffers in D2 SRAM1 (non-cacheable via MPU) ----------
     Usage:  uint8_t rx[64] __attribute__((section(".dma_buf"), aligned(32)));
     ALWAYS align to 32 and size to a multiple of 32 (cache line).        */
  .dma_buf (NOLOAD) :
  {
    . = ALIGN(32);
    _s_dma_buf = .;
    *(.dma_buf)
    *(.dma_buf*)
    . = ALIGN(32);
    _e_dma_buf = .;
  } >RAM_D2_M7

  /* ---- Ethernet descriptors + buffers in D2 SRAM3 -----------------------
     ETH DMA descriptors MUST live in D2 SRAM. Non-cacheable via MPU.     */
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

  /* ---- BDMA / D3 peripheral buffers in SRAM4 ----------------------------
     BDMA reaches ONLY SRAM4 and D3 peripherals (LPUART1, SPI6, I2C4).    */
  .d3_ram (NOLOAD) :
  {
    . = ALIGN(32);
    _s_d3_ram = .;
    *(.d3_ram)
    *(.d3_ram*)
    . = ALIGN(32);
    _e_d3_ram = .;
  } >RAM_D3

  /* ---- Shared memory M7 <-> M4 ------------------------------------------
     Mirrored by the same section in stm32h755zi_cm4.ld at the SAME address.
     Declare contents ONLY through shared_memory.h. Guard with HSEM.
     MUST be non-cacheable (or the M7 will read stale data).              */
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
