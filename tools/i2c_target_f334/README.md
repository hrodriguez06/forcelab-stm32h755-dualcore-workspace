# i2c_target_f334: the I2C target of the 05_i2c bench

Bench firmware for a **NUCLEO-F334R8** used as the I2C target of the 05_i2c
driver bench (`01_shared_drivers/05_i2c/IMPLEMENTATION_PLAN.md`, deviations D1
and D2). It is an instrument, not a catalogue project: bare metal on the CMSIS
device files in `cmsis/` (cmsis_device_f3 v2.3.9, Apache-2.0, see
`cmsis/README.md`), with `core_cm4.h` from the workspace's STM32CubeH7 CMSIS.

## Pins

| F334 | Function | Nucleo | Bus |
| --- | --- | --- | --- |
| PB8 | I2C1_SCL, AF4, open-drain | CN5-10 (D15) | SCL node, to H755 PB8 (CN7-2) |
| PB9 | I2C1_SDA, AF4, open-drain | CN5-9 (D14) | SDA node, to H755 PB9 (CN7-4) |
| GND | | CN5-7 | H755 CN8-11 (wire W3) |
| PA2/PA3 | USART2 TX/RX, AF7 | ST-LINK VCP | console, 115200 8N1 |

The pull-ups (1 kΩ) go to the H755's 3V3; no supply rail joins the two boards
(plan S20).

**S15:** PB8 and PB9 are only ever input without pull, AF4 open-drain without
pull, or GPIO open-drain without pull during a hold. `OTYPER = 1` is written
before `MODER` leaves input; `PUPDR` is never set. The firmware checks the
registers after every change and in its main loop; on a violation it puts both
pins in input, turns I2C1 off and prints `I2C_TARGET ABORT S15`. The banner,
`LEVELS` and `HOLD_END` print `MODER/OTYPER/PUPDR/AFR` of both pins.

**S21:** `HOLD` drives one line low (open-drain) for 1 to 500 ms. TIM2 channel 1
releases it in its interrupt at the limit, whatever the main loop does; with
`EDGES n` (SDA only) the EXTI8 interrupt releases it at the n-th rising edge of
SCL first. The independent watchdog (about 400 ms, refreshed by the main loop
only) resets the chip, and with it puts every pin in input, if the firmware
stops; the fault handlers release the pins before waiting for it. A transfer
that makes no progress for 500 ms is cut with `PE = 0` (`ev` bit `0x08`).

## Build and flash

```powershell
.\tools\i2c_target_f334\build.ps1 -Clean          # 0 warnings expected; prints the sizes
.\tools\i2c_target_f334\build.ps1 -Clean -Werror   # the same with -Werror
```

Flash with this directory as the current one (so `tools/flash.py` takes it as
the project), and ALWAYS with the F334 ST-LINK's serial number (plan S18):

```powershell
cd tools\i2c_target_f334
py ..\flash.py --sn 066BFF393731435243254117 --elf build\f334\i2c_target_f334.elf
```

`flash.py` prints its H755 header lines ("Target: STM32H755ZI", "CPU1 bank 1")
whatever the ELF; the probe line and CubeProgrammer's own output (device
0x438) show what was programmed. Flash or reset only with the bench host
stopped (S20).

Rev and build id come from the same `cmake/fw_build_id.cmake` as prj_01, over
`build/f334/CMakeCache.txt` (`FW_PRESET = f334`): the host recomputes the id.

## Console protocol (v1)

One command per line (CR or LF). Every command gets exactly one reply line,
`OK ...` or `ERR <command> <reason>`; `HOLD_END` and `RPT` lines come by
themselves. Numbers are decimal, or hexadecimal with `0x`.

| Command | Effect |
| --- | --- |
| `ID` | prints the banner again (below) |
| `LEVELS` | `LVL scl=<0/1> sda=<0/1> busy= pe= fmp= PB8 MODER= OTYPER= PUPDR= AFR= PB9 ... s15=ok` |
| `STATUS` | totals since `CLR`, the pointer, the armed faults |
| `CLR` | clears the totals and the report numbering |
| `INIT` | `reg[i] = i ^ 0xA5`, pointer 0 |
| `DUMP <a> <n>` | `n` (1..32) registers from `a` |
| `NACK <k>` | the next transfer NACKs its received data byte `k` (0 = the first, the pointer byte); that byte is not stored |
| `CORRUPT <k>` | the next transfer inverts its returned byte `k` |
| `STRETCH ADDR <us>` | the next transfer holds SCL `us` (0..5000) at the address match |
| `STRETCH BYTE <us>` | the next transfer holds SCL `us` after each byte (byte control, before the ACK bit in reception) |
| `DISARM` | clears every armed fault |
| `FMP 0\|1` | `SYSCFG_CFGR1.I2C_PB8_FMP/I2C_PB9_FMP` (Fm+ drive), read back |
| `HOLD SDA\|SCL <ms> [EDGES <n>]` | I2C1 off, the line driven low now; released at `ms` (1..500) or at the `n`-th SCL rising edge |

Armed faults apply to the NEXT transfer only (the first address match takes
them). `NACK` and `STRETCH BYTE` switch on slave byte control (`SBC = 1`,
`RELOAD = 1`, `NBYTES = 1`) for that transfer; without them every byte is
acknowledged by the hardware with no software in the clock path.

Registers: 256, `reg[i] = i ^ 0xA5` at start. In a write, the first data byte
after the address sets the pointer and the next ones are stored from there; a
read returns the registers from the pointer on. The pointer wraps at 256 and
moves only for bytes that went on the bus (a byte left in `TXDR` when the
master NACKs is taken back).

### Banner

```text
I2C_TARGET v1 Rev <git rev> id <build id>
I2C_TARGET pins PB8 MODER=2 OTYPER=1 PUPDR=0 AFR=4 PB9 MODER=2 OTYPER=1 PUPDR=0 AFR=4 s15=ok
I2C_TARGET addr=0x42 sysclk=64000000 timingr=0x00A20000 idcode=0x... uid=... reset_csr=0x... hold_max_ms=500
I2C_TARGET ready
```

`reset_csr` is `RCC_CSR` at boot (0 when printed by `ID`): bit 29 is an IWDG
reset.

### Report after each transfer

From the first address match to the STOP (or the cut):

```text
RPT n=<seq> st=<address matches> sr=<repeated starts> sp=<1: STOP> dir=<W/R per match>
    rx=<bytes received> tx=<bytes sent> nk=<NACKs given> nm=<NACKs received>
    crc_rx=<CRC-32> crc_tx=<CRC-32> p0=<pointer before> p1=<pointer after> ev=<flags> us=<duration>
```

(one line). CRC-32 is zlib's (`zlib.crc32`) over the bytes in bus order.
`ev`: `0x01` BERR, `0x02` ARLO, `0x04` OVR, `0x08` cut after 500 ms without
progress, `0x10` a write ended with no data byte. `us` is measured with TIM2
(the F334's HSI, ±1 %).

### Hold

```text
OK HOLD line=SDA ms=50 edges=0
HOLD_END line=SDA ms=50 us=<held> edges=<SCL rising edges seen> cause=TIME|EDGES|ABORT scl=<0/1> sda=<0/1> PB8 ... s15=ok
```

`scl`/`sda` are read 1 ms after the release, before the pins go back to AF4.
