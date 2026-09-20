# Display & Graphics (LTDC, DMA2D, DSI, JPEG) - STM32H755

## 1. Documentation
- `19_chrom-art-accelerator-controller-dma2d.pdf` (Hardware 2D image blitting, color conversion).
- `33_lcd-tft-display-controller-ltdc.pdf` (Parallel RGB interface, dual-layer alpha blending).
- `34_dsi-host-dsi.pdf` (MIPI DSI display interface).
- `35_jpeg-codec-jpeg.pdf` (Hardware JPEG encoder and decoder).

## 2. Framebuffer Memory Placement
- Framebuffers require high bandwidth. Typical placement:
  - Internal: AXI SRAM (`0x24000000`, 512 KB) for small displays (e.g. 480x272 16bpp).
  - External: FMC SDRAM (`0xC0000000`) for larger displays (e.g. 800x480, 1024x600).
- DMA2D (Chrom-ART) accelerates pixel blending directly between memory layers without CPU overhead.
