# T20 camera input and ISP: hardware notes (2026-10-27)

What our ISP driver (`uvc/kernel/patches/0012`) is written from. These are facts
about the hardware: addresses, register fields, values, sequences. They were
taken from the documentation-like parts of Ingenic's T20 SDK (register headers,
the open parts of its ISP driver) and checked against ARM's mainline Mali-C55
driver, a later ISP of the same Apical family. No code was copied from the SDK,
open-tx-isp or OpenIMP; those are documentation here, not sources. Nothing below
is verified on hardware.

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
PA15 function 1 is MCLK. This is the vendor's `DVP_PA_LOW_10BIT`, the
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
| 0x90-0x9c | state, pixel and line counters, output FIFO count | read only |

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
  in bit 15, reset values are a limited-range BT.601 matrix), 0x6a4-0x6ac
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

Reset values make a usable minimal pipeline: identity colour matrix, white
balance gains 1.0, digital gain 1.0, black levels 0, BT.601 colour space matrix.
The blocks whose tables are in memory (shading, Iridix, gamma, sinter/temper
noise profiles) must be bypassed until their tables are loaded.

## Not known yet

- The interrupt source numbers of frame start/end, statistics and errors.
- The memory tables (gamma, shading, Iridix, noise profiles) and their formats:
  the vendor loads them from the sensor tuning file through closed code.
- The statistics layout (AE histogram, AWB zones) and the default config
  sequence's format (`SENSOR_ISP_SEQUENCE_DEFAULT`).
- Whether the ISP core needs a reset or clock sequence beyond the two clocks.
