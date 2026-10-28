# T20 camera input and ISP: hardware notes (2026-10-27)

What our ISP driver (`uvc/kernel/patches/0012`) is written from. These are facts
about the hardware: addresses, register fields, values, sequences. They were
taken from the documentation-like parts of Ingenic's T20 SDK (register headers,
the open parts of its ISP driver) and checked against ARM's mainline Mali-C55
driver, a later ISP of the same Apical family. No code was copied from the SDK,
open-tx-isp or OpenIMP; those are documentation here, not sources. Verified on
a Wyze Cam v2 (JX-F22) on 2026-10-28: the clocks, the sensor pins, the VIC start
sequence for DVP RAW10, the DS1 path with the reset-value pipeline, the NV12
writers and the DS1 output-end interrupt (source 50 on line 13) give 1920x1080
frames at 25 fps. Verified on 2026-10-28 too: the black level, white balance,
colour matrix, DS1 gamma, colour space converter, AWB statistics and Sinter
registers below, and the sensor's mirror and flip. The rest is not verified.

The register map's source is Ingenic's T20 SDK header
`include/apical-isp/apical_isp_config.h` (with the `*_mem_config.h` headers for
the tables), as published in themactep/ingenic-sdk (`common/isp/t20`).

## Blocks

| Block | Address | Size | Notes |
|---|---|---|---|
| VIC (video input controller) | 0x13300000 | 0x10000 | sensor interface (DVP or MIPI), feeds the ISP core; no memory output |
| ISP interrupt controller | 0x13320000 | 0x10000 | collects the core's 16 interrupt lines and the VIC's |
| ISP core (Apical) | 0x13380000 | 0x80000 | 897 documented registers, tables after them |
| MIPI CSI | 0x10040000 | 0x1000 | unused on the Wyze Cam v2 (parallel sensor) |

INTC line 29 is the ISP interrupt. Clocks: `isp` gate (CLKGR0 bit 23) and the
ISP divider `cgu_isp` (ISPCDR, the vendor runs it at 100 MHz with
`isp_clk=100000000`); the sensor clock is `cgu_cim` (CIMCDR) at 24 MHz on PA15.

## Sensor pins (DVP, 10 bit, "low" alignment)

Function 1 on PA0-PA9 (data), PA14 (pixel clock), PA16 (HREF), PA17 (VSYNC);
PA15 function 1 is MCLK. The sensor does not answer on I2C until MCLK runs on
PA15. This is the vendor's `DVP_PA_LOW_10BIT`, the
`sensor_gpio_func=0` that S50camera passes today. Sensor reset is PA18 (GPIO 18),
low active.

## VIC (T20 layout)

| Offset | Register | Use |
|---|---|---|
| 0x00 | CONTROL | bit 0 start, 1 register enable, 2 global reset, 3 safe reset, 4 VIC reset |
| 0x04 | RESOLUTION | width << 16 \| height |
| 0x0c | INTF_TYPE | 0 BT.656, 1 BT.601, 2 MIPI, 3 DVP, 4 BT.1120 |
| 0x10 | DB_CFG (DVP) | bits 19:17 data type (1 = RAW10), bit 20 high alignment, bit 0 VSYNC polarity low, bit 1 HSYNC polarity low |
| 0x14 | IDI_TYPE | MIPI data type |
| 0x50 | GLOBAL_CFG | ISP preset mode in bits 6:5 (0 = mode 1) |
| 0x90-0x9c | state, pixel and line counters, output FIFO count | read only; while streaming 1080p: 0x90 = 3, 0x94 = 0x059e01e2, 0x98 = 0x00e20356, 0x9c = 0x4f0 (one sample, not decoded) |

Start for a DVP RAW10 sensor: INTF_TYPE = 3, DB_CFG = RAW10 (polarity bits
only when the sensor's are low), RESOLUTION, GLOBAL_CFG = 0, CONTROL = 2
(register enable), then CONTROL = 1 (start). Stop: CONTROL = 8 (safe reset).

## ISP interrupt controller

| Offset | Register |
|---|---|
| 0x04 | clear: write the pending bits |
| 0x0c | status |
| 0x14 | enable (bits set = enabled) |
| 0x1c | mask (bits set = masked) |

Bits 15:0 are the ISP core's interrupt lines 0-15, bits 22:16 the VIC's.
Pending = status & ~mask.

## ISP core

All offsets below are from 0x13380000.

The register map (offsets from 0x13380000) is the Apical one; the groups the
driver uses:

- **Top**: 0x10 active width, 0x14 active height, 0x18 Bayer start (3 = BGGR,
  the JX-F2x order), 0x30 config buffer mode (2 = registers latch in vertical
  blanking), 0x40/0x44 per-block bypass bits:
  0x40 bit 1 mirror, 2 sensor offset, 3 digital gain, 4 frontend LUT, 5 RAW
  frontend, 6 defect pixel, 7 frame stitch, 9 sinter, 10 temper, 12 white
  balance, 13 radial shading, 14 mesh shading, 15 Iridix, 16 demosaic, 17 colour
  matrix, 18-22 FR crop/gamma/sharpen/logo/colour space, 24-29 the same for DS1
  (25 is the DS1 scaler), 30 DS1 DMA source (0 = colour space converter);
  0x44 bits 1-6 the DS2 path.
- **Interrupts**: 0x80-0x9c, sixteen 6-bit source selectors (line n at
  0x80 + 4 × (n / 2), bits 5:0 for even n, 21:16 for odd); 0xa0 status, 0xa4
  clear (a 0 → 1 write clears). Known sources: 50 = DS1 output end, 53 = DS2
  output end. The vendor routes DS1 output end to line 13 and DS2 to line 14.
  The other source numbers are not documented in the SDK.
- **Input port**: 0x100 = 0x00100001 for this sensor (preset 1, vertical counter
  on the falling edge of HS); 0x130 bits 2:0 mode request (1 safe start, 0 safe
  stop), 0x134 mode status.
- **Colour space converter, DS1**: 0x680-0x6a0 the 3×3 RGB → YCbCr matrix (sign
  in bit 15; the reset values, 0x2f 0x9d 0x10 in the Y row, are BT.709 luma
  scaled to 219/255 with no Y offset), 0x6a4-0x6ac
  offsets, 0x6b0 bit 0 matrix, 1 filter, 2 horizontal and 3 vertical chroma
  downsampling (all needed for 4:2:0).
- **Video DMA writers**: DS1 Y at 0xc00, DS1 UV at 0xc80 (FR 0xb00/0xb80,
  DS2 0xd00/0xd80, same layout):
  +0x00 bits 3:0 base mode (13 = NV12), 7:6 plane (0 Y, 1 UV, 2 VU), bit 9
  frame write on; +0x04 active width | height << 16; +0x08-0x18 bank 0-4
  base addresses; +0x1c bits 2:0 highest bank, bit 3 restart at bank 0;
  +0x20 line offset (bytes); +0x24 bit 1 AXI port enable, bits 10:8 current
  bank; +0x38 alarms.
  Same layout and codes as Mali-C55's writers.
- **No full-resolution channel on the T20**: the vendor driver bypasses every FR
  block on this SoC (`TX_ISP_EXIST_FR_CHANNEL == 0`); the main stream is DS1 with
  its scaler bypassed, the second stream DS2.

- **Sensor offset** (black level, before the gains): 0x140/0x144/0x148/0x14c
  for the R, Gr, Gb, B positions, 12-bit units; on with bypass bit 2 clear.
  The JX-F22's level is 40 (measured at the shortest exposure: Y is uniform,
  and the same at analog gain codes 0, 40 and 79).
- **White balance**: 0x300/0x304/0x308/0x30c gains for the same positions,
  4.8 fixed point (0x100 = 1), 12 bits. "00" is the red pixel whatever the
  Bayer start: raising 0x300 warms the picture. 0x310-0x31c is a second black
  offset after the gains (also 12-bit units).
- **Colour matrix**: 0x480-0x4a0 row-major 3x3 on linear RGB, sign and
  magnitude (bit 15), 1/256; 0x4a4 bit 0 enable. Reset: identity.
- **DS1 gamma**: LUT of 129 12-bit entries at 0x11000 (one per 32-bit word),
  input evenly spaced; 0x4e4 bit 0 enable, bit 1 CPU priority (reset 1: CPU
  writes always land), bit 2 ready. On with bypass bit 26 clear. Written before
  the stream starts; reads back while idle.
- **Colour space converter, DS1**: the coefficients are sign and magnitude,
  1/256, on 10-bit values; the offsets 0x6a4-0x6ac are 11-bit two's
  complement (0x7c0 = -64). The reset matrix has no Y offset (black is Y 0).
- **AWB statistics**: 0x868 bit 0 mode (0: G/R and G/B, 1: R/G and B/G),
  0x858 and 0x85c the two ratios (4.8 fixed point, 12 bits), 0x860 the pixel
  count, for the whole frame, updated every frame. 0x4c selects the tap: 0
  (reset) is before the colour matrix, after the white balance gains. 0x840
  and 0x844 bound the pixel values counted (10 bits), 0x848-0x854 the ratios
  (defaults 0.25 to 2).
- **AE statistics**: 0x820-0x82c a normalised 5-bin histogram (bins 0, 1, 3, 4;
  bin 2 is the rest), thresholds at 0x800-0x80c; 0x48 selects the tap.
- **Sinter** (spatial noise reduction): bypass bit 9; 0x280 config (reset
  0x41c: on); 0x290-0x29c the thresholds of bands 0, 1, 2, 4, horizontal and
  vertical, 8 bits in bits 7:0 and 23:16; 0x2ac-0x2b8 strengths (reset 0xff);
  the noise profile LUT at 0x1d00, 128 8-bit entries packed four per word.
  A flat profile of 32 with thresholds of 32 removes most of the colour noise
  at gain code ~60; 128 and up smears detail.
- **DS2 path**: source 0x44 bit 0 (0, reset: after the colour matrix, so
  independent of DS1); bypass 0x44 bits 1 crop, 2 scaler, 3 gamma, 4 sharpen,
  5 logo, 6 colour space converter. Its gamma LUT is at 0x12000 (enable
  0x4e8), its converter at 0x780 (DS1's layout + 0x100), its writers at
  0xd00/0xd80, its output end is source 53 (routed to line 14). Verified at
  640x360 and 320x180 from 1080p, alone and with DS1 streaming.
- **DS2 scaler** at 0x740: +0x08/+0x0c input width and height, +0x10/+0x14
  output, +0x18 horizontal and +0x20 vertical increment (input / output in
  4.20 fixed point), +0x1c/+0x24 the filter set (0-3). The filters are in
  memory: horizontal at 0x18000, vertical at 0x1a000 (DS1's at 0x14000 and
  0x16000), 0x100 bytes a set: 32 phases of 8 signed 8-bit taps (two words,
  tap 0 in the low byte), each phase summing to 64. Phase p puts the output
  sample at tap 3.5 - p/32 horizontally; the vertical filters hold the same
  taps in reverse order (tap 7 is 0). Empty at reset, so they must be loaded.
- **Bayer start** (0x18): 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR (all four verified
  with the sensor's mirror and flip).

Reset values make a usable minimal pipeline: identity colour matrix, white
balance gains 1.0, digital gain 1.0, black levels 0, the BT.709-like colour
space matrix above.
The blocks whose tables are in memory (shading, Iridix, gamma, sinter/temper
noise profiles) must be bypassed until their tables are loaded.

## Sharpening (verified 2026-10-28)

Each scaled output has a sharpening block (the Apical "Sharpen DS1/DS2"
groups): 0x620 (DS1; DS2 at 0x720) bit 0 on, bits 3:2 coring (1 at
reset), 0x624 (0x724) bits 7:0 strength (0x30 at reset), 0x628 (0x728)
controls R and B (0x60, 0x40). The blocks are bypassed by 0x40 bit 27 and
0x44 bit 4. No table to load: on at the reset strength, the picture is
slightly crisper with no visible noise gain; at 0xa0 the sensor noise is
amplified. The demosaic's own sharpening (0x43c directional, 0x440
non-directional, signed 4.4, 0x30 and 0x20 at reset) is left at reset.

## Sensor (JX-F22) mirror and flip

Register 0x12: bit 5 mirror, bit 4 flip, bit 6 sleep. Mirror turns BGGR into
GBRG, flip into GRBG, both into RGGB.

## Not known yet

- The interrupt source numbers of frame start/end, statistics and errors.
- The shading and Iridix tables and their formats; what the noise
  profile entries mean exactly (the vendor's are calibrated per sensor).
- The zoned statistics (AE and AWB per zone, in the metering memory at
  0x8000) and the default config sequence's format
  (`SENSOR_ISP_SEQUENCE_DEFAULT`).
- Whether the ISP core needs a reset or clock sequence beyond the two clocks.

## VPU JPEG encoder (verified 2026-10-28)

The VPU at 0x13200000 (1 MiB) is the same family as the X1000's and the
T31's ("helix"): Ingenic's GPL-2.0 kernel drivers for those (Ingenic-community
linux `drivers/media/platform/ingenic/jpeg-x1000`, the T31 SDK's
`ingenic-vcodec/helix`) program the same scheduler, VDMA, EFE and JPGC
registers as the T20's `jzm_vpu.h` lists (137 registers at the same
addresses). That header's MCE and VMAU are the older JZ4780/T10 ones: the
T20 has the helix ones (see the H.264 section).

- Interrupt: INTC 62. Clocks: gate CLKGR1 bit 0 (`vpu`), divider VPUCDR
  (`cgu_vpu`, the vendor runs it at 350 MHz). Reset: CPM SRBC (0x100000c4)
  bit 30 stop request, bit 29 stop acknowledge, bit 31 reset.
- The VPU runs a descriptor list: pairs of (value, VLD bit 31 | TERM bit 30 |
  register offset), started by writing its address | 1 to 0x10008 (128-byte
  aligned). Scheduler 0x0 GLBC: bit 9 HIAXI, bits 16-20 interrupt enables;
  0x34 status: bit 0 end, bit 4 JPEG end.
- JPEG: Huffman encoder table at 0xe1800 (384 entries, (length - 1) << 8 |
  low 8 bits of the code; Annex K), quantisation memory at 0xe1400 (luma 64
  then chroma 64 entries, zigzag order, each s << 11 | round(2^(11+s) / q):
  every pair in the vendor's tables fits this; q = 1 does not, so q >= 2).
  The bitstream address must be 256-byte aligned; the hardware writes only
  the entropy-coded data, and 0xe0008 bits 23:0 give its length: the headers
  and EOI are the driver's. EFE in JPEG mode reads NV12 (plane mode 2) a
  macroblock at a time into the VPU's RAM at +0xf0000, so frames need room
  for whole 16-line rows (1080 -> 1088).
- 1920x1080 at quality 80: 10.7 ms, PSNR 45 dB against the source.


## VPU H.264 encoder (verified 2026-10-28)

The T20's H.264 encoder is the Helix generation of the VPU (as the T21/T23/
T30), not the JZ4780/T10 one ("NVPU"). The T10 and T20 run the same vendor
libimp (3.12.0), which picks `H264E_T10_SliceInit` or `H264E_T20_SliceInit`
from the SoC id. A T10 descriptor list (old MCE at 0x50004..0x5084c, scratch
in the TCSM at 0xc4000-0xc5200 in T10 order) runs on a T20 to its end but
the encoder never finishes: the EFE takes the frame, the SDE writes nothing.

Sources: the register meanings and field layouts are those of the GPL-2.0
Helix headers (T31 SDK `ingenic-vcodec/helix/api/helix.h`); the register
values and the order of the T20 list are as OpenIMP records them from the
vendor's `H264E_T20_SliceInit` (`src/t30/t30_h264_descriptor.c`, the
`PLATFORM_T20` branches; documentation only), checked on this camera.

- One slice per frame. Descriptor list, in order (offsets from 0x13200000;
  "TCSM" = 0x132c0000 + offset, "SCHEn" = 0x13200070 + 4 (n - 1)):
  - 0xc0000 = 0 (TCSM flush).
  - EFE: 0x40004 geometry (first row << 24 | last row << 8 | last column),
    0x4000c = TCSM 0x0 (residuals), 0x40010/0x40014 raw Y/C, 0x40034 = 0,
    0x40038 = Y stride << 16 | C stride, 0x40018 top MVs = TCSM 0x4800,
    0x4001c top prediction modes = TCSM 0x4000, 0x40020 ME channel = TCSM
    0x5400, 0x40024 VMAU channel = TCSM 0x4c00, 0x40028 DBLK channel = TCSM
    0x5000, 0x4002c SDE channel = TCSM 0x5600, 0x40030 = 0x132f0000 (VRAM,
    the macroblock FIFO), 0x40040 max QP, 0x40044/0x40048 ROI regions
    (qp << 2 | mode << 1 | on, a byte each), 0x4004c..0x40068 ROI positions
    (bottom, top, right, left row/column bytes), 0x4006c macroblock QP table
    (on << 31 | words << 20 | TCSM 0x5800), 0x40120 chroma QP offset (5
    bits; the PPS must match), 0x40108 = 0, 0x4010c = 0x03400000.
  - MCE, P frames only: 0x5010c = TCSM 0x5400, 0x50104 = TCSM 0x4800, 0x50108
    = SCHE1, 0x50060 = height - 1 << 16 | width - 1 (pixels), 0x50064 =
    stride C << 16 | Y (16 * macroblocks across), 0x50010 = 0x800a8021
    (chroma 2-tap bilinear eighth pel, luma 6-tap quarter pel), 0x50040 =
    0x873f5008 (lambda scale 8, predictors, 63 steps, quarter and half pel
    refinement, 16x16), 0x50044 = 0, 0x50048 = 0x07d007d0 (MV range 500
    pixels), 0x50800/0x50804 reference Y/C, then the interpolation tables
    (luma 0x50900 + 8 i two words, 0x50a00 + 16 i four tap words; chroma
    0x50980 + 8 i, 0x50b00 + 16 i and + 8): the GPL IntpFMT H264_QPEL and
    H264_EPEL entries packed as MCE_ILUT_INFO (clip and idgl from hldgl) and
    MCE_CLUT_INFO, then 0x50068 = 0, 0x5004c = 0, 0x50000 = 0x11.
  - VMAU: 0x80040 = 4 (reset), 0x80050 = 0x80000b00, 0x8000c = TCSM 0x4c00,
    0x8005c = 0x132ffffc, 0x80058 = SCHE2, 0x80054 = aligned height << 16 |
    width, 0x80044 = 0x01000001, 0x80078 = 0x0b150b15 (deadzones), 0x80028 =
    TCSM 0x4000, 0x80030 = last row << 24 | last column << 16 | (P 2, I 1),
    0x80034 = 0x00008202, 0x80038 = 0xcccc0111, 0x8003c = 0, 0x800d8 = 0,
    0x800dc = 0, 42 writes to the FIFO 0x8002c of the initial CABAC states
    (preCtxState, 1-126) of contexts 3-12 (I) or 14-23 (P), 73-84, 64-69,
    40-53; 0x8007c = 2; 0x80040 = 1 (run).
  - DBLK: 0x70060 = 4, 0x70000 = TCSM 0x5000, 0x70078 = SCHE3, 0x70074 =
    rows << 16 | columns, 0x7007c = first row << 16, 0x70064 = 1 (filter, no
    border), 0x70084/0x70088 reconstructed Y/C, 0x7008c = 0x132ffffc,
    0x70080 = 128 columns << 16 | 256 columns, 0x70068 = P << 3 | 1,
    0x70060 = 8.
  - SDE: 0x90000 = 0, 0x9000c = 0x11, 0x90008 = rows << 24 | columns << 16 |
    first row << 8, 0x90010 = 2, 0x90014 = 3, 0x90018 = QP << 8 | 0x10 |
    (P 2, I 1), 0x9001c = TCSM 0x5600, 0x90020 = SCHE4, 0x90024 the
    bitstream (256-byte aligned), 460 context words at 0x92000 (LPS ranges
    of pStateIdx | valMPS, as XBOMX's table), 0x90004 = 2.
  - Scheduler: 0x60..0x7c = 0, 0x60 = 0x0c0c0400 | P << 2, 0x64 =
    0x97850fce | P, and last (TERM) 0x40000 = 0xc0000000 | QP << 8 | P << 4 |
    8 (NV12) | 0x23.
- Reconstructed frames (the next frame's reference) are tiled: macroblocks
  one after the other, 16x16 luma (256 bytes) and 8 lines of 8 Cb then 8 Cr
  bytes (128), no border, chroma after the luma: 1.5 x 16-aligned size.
- The VPU writes the slice data only (CABAC, with its end and the RBSP stop
  bit); 0x90038 bits 23:0 give its length. The software writes the SPS, PPS
  and slice header up to cabac_alignment_one_bit and adds the emulation
  prevention bytes.
- Interrupt as for JPEG (0x34 bit 0); 1920x1080: 16.3 ms in the VPU (P
  frames), 640x360: about 2 ms.

## Audio: AIC and internal codec (verified 2026-10-28)

The AIC (I2S) at 0x10020000 (interrupt 1) is the JZ4780/X1000 generation:
AICFR FIFO thresholds at 27:24 (receive) and 20:16 (transmit), I2SDIV
playback 3:0 and capture 11:8, DMA requests 6 (I2S0 transmit) and 7
(receive) on the PDMA at 0x13420000 (interrupt 10, 32 programmable
channels, as the X1830's). Sources: Ingenic's GPL-2.0 vendor kernel (the
Xiaomi Dafang tree's `sound/oss/jzsound`, `soc-t20` headers).

- The internal codec is memory mapped at 0x10021000 (registers every 4
  bytes, the low byte used, up to 0x130). It is the I2S master (CMCR =
  0x3e) and runs on the AIC's system clock: AICFR bit 7 (ICS) selects the
  internal codec, bit 5 (ICDC) makes it master, I2SCR bit 4 outputs the
  system clock. The vendor sets the clock (CGU I2SCDR 0x60: M bits 21:13,
  N bits 12:0, bit 29 divider on, bit 30 divider selected, bit 31 MPLL
  rather than SCLK_A; I2SCDR1 0x70 written 0 after N changes, as on the
  X1000) to 256 fs; the sample rate is CSRR (0x110) bits 2:0, 7..0 for 8,
  12, 16, 24, 32, 44.1, 48, 96 kHz.
- Power-up: CGR (0x00) bits 1:0 to 0 then 3; CMCR 0x3e, CDCR1 0x10, CDCR2
  0x0e; CHCR (0x9c) bits 7:6 = 1 (precharge); on the T20 CCR (0xa0) bit 7
  set, then bits 6:0 stepped up to 0x3f over about 200 ms.
- Microphone: CACR (0x08) bits 6:5 sample length, 4:3 = 2 (I2S), bit 0
  mono; CACR2 (0x8c) bits 4:0 PGA (-18 dB + 1.5 dB steps), bit 5
  single-ended; CAACR (0x84) bit 7 current source, bit 6 bias, bits 2:0
  bias voltage (6: 1.6 VCC), bit 5 zero crossing; CAMPCR (0x90) bits 7, 6,
  5 reference, ADC clock, ADC amplifier; CMICCR (0x88) bit 6 boost on, bit
  5 +20 dB, bit 4 boost running, bits 1:0 ALC on and running. Off: CAMPCR
  bits 7:5 cleared.
- The one ADC's samples are in the right I2S channel; the left channel has
  a copy of every 8th sample and zeros between (in mono mode as in stereo):
  the vendor sums the two in software, we take the right one.
