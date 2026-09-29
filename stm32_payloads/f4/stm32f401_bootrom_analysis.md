# STM32F4 (F401xD/E, DEV_ID 0x433) Boot ROM Analysis

Companion to `../f1/stm32f1_bootrom_analysis.md`. The headline result:

> **The F4 system bootloader performs NO software RDP check on the Read Memory
> command.** Where the F1 bootloader explicitly reads `FLASH_OBR.RDPRT` and NACKs
> a read when RDP is set, the F4 read path only validates the *address region* and
> leaves read-protection entirely to the **flash-controller hardware**. There is
> no RDP branch instruction to glitch or side-step in the F4 read path.

Dump source: `0x1FFF0000`, 30 KB (0x7800), read over SWD from an F401 at **RDP0**
(mass-erased via bootloader Readout-Unprotect first — at RDP1 the whole MEM-AP
faults, ACK=0x4, so the ROM can't be read locked). Artifacts: `stm32f401_bootrom.bin`
(raw) and `stm32f401_bootrom.S` (objdump `-Mforce-thumb`, vma 0x1FFF0000).

## Vector table (0x1FFF0000)

| Offset | Value | Purpose |
|--------|-------|---------|
| 0x000 | 0x20002E48 | Initial SP |
| 0x004 | 0x1FFF4253 | Reset vector (Thumb → 0x1FFF4252) |
| 0x008.. | 0x1FFF425F | NMI / fault handlers (repeated stub) |

## USART command loop (0x1FFF1832)

The USART bootloader dispatcher. `r6` is loaded once with **`FLASH_OPTCR`
(0x40023C14)** and kept live for the whole loop; `r7 = 0xFFF0`. Each iteration
kicks the IWDG (writes `0xAAAA` to `IWDG_KR` 0x40003000, literal @0x1FFF1C3C),
reads command byte + complement (`eor`/`cmp #0xFF`), then dispatches:

| Cmd | Name (AN3155) | Handler |
|-----|---------------|---------|
| 0x00 | Get | — |
| 0x01 | Get Version | — |
| 0x02 | Get ID | — |
| **0x11** | **Read Memory** | **0x1FFF18E6** |
| 0x21 | Go | 0x1FFF1960 |
| 0x31 | Write Memory | 0x1FFF1998 |
| 0x44 | Extended Erase | 0x1FFF1AB8 |
| 0x63 | Write Protect | 0x1FFF1B3C |
| 0x73 | Write Unprotect | 0x1FFF1B90 |
| 0x82 | Readout Protect | 0x1FFF1BAA |
| 0x92 | Readout Unprotect | 0x1FFF1BC4 |

## Command map (both dispatchers)

The ROM has **two** command dispatchers:

**USART dispatcher `0x1FFF188E`** — the UART path. Handles exactly the AN3155
set, NACKs anything else:

`00 Get · 01 Get Version · 02 Get ID · 11 Read Memory · 21 Go · 31 Write Memory ·
44 Extended Erase · 63 Write Protect · 73 Write Unprotect · 82 Readout Protect ·
92 Readout Unprotect`

No undocumented/unexpected UART commands. No legacy Erase `0x43` (F4 is
Extended-Erase-only) and no `0x50/0x51` Special commands. The UART **Get** table
at `0x1FFF1D90` confirms it — advertised set == handled set:

```
79 0B 31 | 00 01 02 11 21 31 44 63 73 82 92 | 79
ACK N=11  V3.1  <---------- 11 commands ---------->  ACK
```
→ **bootloader version 3.1**, 11 commands.

**Second dispatcher `0x1FFF01F0`** — a *different interface* (not UART). Handles
the eleven above **plus** the "+1" opcodes `32 45 64 74 83 93`. Those are the
**I2C bootloader "No-Stretch" command variants** (Write / Erase / Write-Protect /
Write-Unprotect / Readout-Protect / Readout-Unprotect — documented in AN4221),
paired with each stretch version. A master table of all 17 opcodes sits at
`0x1FFF0916`. These are **not reachable over UART** (the USART loop never compares
against them).

**Interface init.** Bootloader startup initializes a peripheral table at
`0x1FFF1750`: GPIOA/B/C (`0x40020000/0400/0800`), **I2C1/I2C2/I2C3**
(`0x40005400/5800/5C00`, init paths at `0x1FFF1224/128C/1326`, pin-config at
`0x1FFF14A6/1502/1532`), and SPI1 (`0x40013000`). So this ROM scans I2C (and SPI)
in addition to USART — **even though AN2606 does not list an I2C boot interface
for the F401** (it documents only USART1, USART2, USB DFU). Whether the I2C
handler is actually reachable on F401 silicon is an open, testable question (see
the I2C-access test plan).

## Read Memory (0x11) path — the critical difference

```
0x1FFF18E6  bl 0x1FFF1C44          ; ACK + receive 4-byte address & checksum -> r0 = addr
0x1FFF18EA  bl 0x1FFF1BE4          ; save addr to [sp,#16]; tail-call 0x1FFF0928 (classify)
0x1FFF18EE  bl 0x1FFF1BD8          ; r0 = region code; cmp r0,#1; set Z
0x1FFF18F2  beq 0x1FFF195C         ; region == 1 (invalid) -> NACK (0x1F)
            ...                    ; else stream bytes from addr via 0x1FFF1C20 (send byte)
```

`0x1FFF1BD8` is trivial — `cmp r0,#1; bx lr`. The permission decision is entirely
`0x1FFF0928`, and **it is an address-region classifier, not an RDP check** — it
never reads OPTCR or any RDP bit:

```
0x1FFF0928  r1 = addr - 0x08000000
            if r1 <  0x80000            -> return 2   ; MAIN FLASH  (0x08000000..0x08080000)
            if addr+0xDFFFD000 < 0x15000 -> return 3  ; SRAM        (~0x20003000..0x20018000)
            if addr+0xE0004000 < 0x000F  -> return 4  ; OPTION BYTES(0x1FFFC000..0x1FFFC00F)
            if addr+0xE0010000 < 0x7800  -> return 5  ; SYSTEM MEM  (0x1FFF0000..0x1FFF7800)
            if addr+0xE0008800 < 0x0210  -> return 7  ; OTP         (0x1FFF7800..0x1FFF7A10)
            if addr+0x00010000 < 4       -> return 0xFF
            else                          -> return 1  ; INVALID -> NACK
```

A main-flash address returns **2**, which passes the `cmp r0,#1` gate. The
bootloader therefore *permits* the flash read in software regardless of RDP; the
only reads it refuses are to unmapped addresses (region 1).

The only `FLASH_OPTCR` (0x40023C14) reads in the entire ROM are in the option-byte
**programming** paths (flash unlock / erase / Readout-(Un)protect near 0x1FFF09AC
and 0x1FFF327A), **never** in the Read Memory path.

## Contrast with STM32F1

| | STM32F1 (0x410) | STM32F4 (0x433) |
|---|---|---|
| Read-cmd RDP gate | **Software**: `rdp_check` @0x1FFFF132 reads `FLASH_OBR`, tests `RDPRT` bit; read gate @0x1FFFF59C `beq` NACKs when set | **None in software**: read path only classifies address region (0x1FFF0928); flash region always "allowed" |
| RDP enforcement | Software bootloader branch (+ flash controller) | **Flash-controller hardware only** — blocks flash reads by any non-flash-master (bootloader, SRAM code, debug) at the bus level |
| SRAM code reads flash @RDP1 | **Yes** (F1 controller doesn't block SRAM-master flash reads) | **No** — hardware read-block applies to SRAM/debug masters |
| FPB / SRAM-boot "reader trick" | Works (fools/skips the software path) | **Fails** — there is no software gate to fool; the block is hardware |

## Attack implications

1. **No branch to glitch in the F4 read path.** The classic "glitch the RDP
   `beq` in the bootloader read handler" (viable on F1) has no F4 analogue — the
   read handler contains no RDP conditional.
2. **The FPB/SRAM-boot BYPASS cannot beat F4 RDP1** — confirmed independently by
   the `debug/fpb-probe` measurement (stage1 runs, FPB survives the reset, but the
   locked flash-boot never enters stage2). Both results point at the same wall:
   RDP1 on F4 is a hardware flash-controller read-block, not a software check.
3. **The only bootloader-based F4 route is to glitch the flash-controller read
   itself** — a fast core-rail (VCAP) transient timed on the Read-Memory data
   fetch, so the controller returns data instead of blocking (the Joe Grand /
   mkesenheimer VCAP-crowbar technique). That needs the fast crowbar hardware; the
   slow GPIO-sink brownout cannot produce the sub-µs transient.

## How this ROM was obtained

```
TARGET BL RU WIPE          # readout-unprotect -> RDP0 (mass-erase; required — RDP1 faults MEM-AP)
SWD IDCODE ; SWD SPEED 0   # F4 auto-detected, max SWD clock
# host: dump 0x1FFF0000 x 0x7800 via repeated `SWD READ` (scripts/… dump helper)
```
