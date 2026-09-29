# RDP1 Debug Matrix — STM32F401 (DEV_ID 0x433)

**Headline:** On STM32F4, RDP level 1 is a **narrow flash-domain read block**, not a
debug lockout. Main flash, the system bootrom, and the option bytes are unreadable
over the debug MEM-AP — but **SRAM, every peripheral register, and the entire
core-debug suite (halt / step / registers / DWT / FPB / DEMCR) stay fully
available.** Debug is *not* disabled at RDP1.

Measured 2026-09-29 on the bench F401 at RDP1, using only existing `SWD`
CLI commands (no special firmware). Companion to
`stm32_payloads/f4/stm32f401_bootrom_analysis.md` and `SWD SCAN`.

## Matrix

| Capability | Addr / op | RDP1 | Observed |
|---|---|---|---|
| DP connect / IDCODE | DPIDR | ✓ | 0x2BA01477 |
| AP enum + CoreSight ROM (`SWD SCAN`) | AP IDR / 0xE00FF000 | ✓ | full topology (SCS/DWT/FPB/ITM/TPIU/ETM) |
| Main flash read | 0x08000000 | ✗ | ACK=0x4 (FAULT) |
| System bootrom read | 0x1FFF0000 | ✗ | ACK=0x4 (FAULT) |
| Option-byte read | 0x1FFFC000 | ✗ | ACK=0x4 (FAULT) |
| SRAM read | 0x20000000 | ✓ | `DF F8 38 04` |
| SRAM write | 0x20010000 | ✓ | wrote+verified 0xCAFEBABE |
| Peripheral read | RCC_CR 0x40023800 | ✓ | 0x03007183 |
| CPUID | 0xE000ED00 | ✓ | 0x410FC241 (Cortex-M4) |
| DHCSR read | 0xE000EDF0 | ✓ | 0x03090000 run / 0x00030003 halted |
| DEMCR read+write | 0xE000EDFC | ✓ | wrote VC_CORERESET, verified, cleared |
| DWT registers | 0xE0001000 | ✓ | DWT_CTRL 0x40000000 |
| FPB registers | 0xE0002000 | ✓ | FP_CTRL 0x00000260 |
| DBGMCU | 0xE0042000 | ✓ | 0x10016433 |
| Core HALT / RESUME | DHCSR | ✓ | S_HALT observed |
| Core register read | DCRSR/DCRDR | ✓ | see below |

Registers captured while halted at RDP1 (blank+locked target):
```
r0=0x40020000  r10=0xE000E010  r11=0x40003C00  sp=0x20002E00
pc=0xFFFFFFFE  xPSR=0x81000003   ; exception 3 = HardFault
```
The part had hardfaulted at boot (blank flash after erase) — and that state is
fully visible from the registers **even though flash itself is unreadable**.

## What is / isn't blocked

- **Blocked:** debug MEM-AP reads of the *flash domain* — main flash
  (0x08000000), system memory / bootrom (0x1FFF0000), option bytes (0x1FFFC000).
  (Consistent with the bootrom analysis: RDP is enforced by the flash controller
  against non-flash-master reads, not by a software gate.)
- **Open:** SRAM (R/W), all peripheral register banks, the System Control Space
  (NVIC/SCB/SysTick + fault status), DWT, FPB, DEMCR/vector-catch, DBGMCU, and
  full core control (halt, single-step, register read/write).

## Implications for fault-injection research

1. **Full core instrumentation at RDP1.** Halt / step / register access / DWT
   cycle-count timing / FPB breakpoints all work locked — so the bootrom read
   window can be characterised (CYCCNT, breakpoints) directly on a locked part,
   not only on an unlocked one.
2. **Observe a glitch while locked.** `VC_CORERESET` works at RDP1: arm
   halt-on-reset, glitch the boot, then snapshot PC / registers / SRAM /
   peripherals to see what the fault flipped — no flash read needed. This makes
   the "fault-effect snapshot/diff per glitch" approach feasible on a locked
   device for all non-flash state.
3. **Flash content still needs the transient.** The flash block is on the debug
   and non-flash-master paths, so reading actual flash still requires the timed
   VCAP bootloader-read glitch — but the whole debug suite above is available at
   RDP1 to trigger and measure it (debug-attach disturbs neither SRAM, peripheral,
   nor core access — only flash).

---

# Proposed tooling: `SWD SNAPSHOT`

A diffable capture of the target's non-flash observable state, for cataloguing
what a glitch changes (works at RDP0 and RDP1). Pre/post snapshots diff cleanly
with `diff`.

## Syntax

```
SWD SNAPSHOT [sram_addr] [sram_len]
```
- `sram_addr` (default `0x20000000`), `sram_len` bytes (default `256`, max e.g. 4096).
- Auto-connects (like other SWD commands). Halts the core for a coherent capture,
  then restores the prior run/halt state on exit.

## Captured state

- **Core registers** (via the existing REGS path): r0–r12, sp (MSP/PSP), lr, pc,
  xPSR, plus CONTROL/PRIMASK/FAULTMASK/BASEPRI.
- **Fault status (SCB):** CFSR 0xE000ED28, HFSR 0xE000ED2C, DFSR 0xE000ED30,
  MMFAR 0xE000ED34, BFAR 0xE000ED38 — the "why did it fault" set.
- **Debug:** DHCSR 0xE000EDF0, DEMCR 0xE000EDFC.
- **Clock / power / flash-iface:** RCC_CR 0x40023800, RCC_CFGR 0x40023808,
  FLASH_ACR 0x40023C00, FLASH_OPTCR 0x40023C14, PWR_CR 0x40007000,
  PWR_CSR 0x40007004.
- **ID:** DBGMCU_IDCODE 0xE0042000.
- **GPIO mode:** GPIOA/B/C MODER (0x40020000 / 0x40020400 / 0x40020800).
- **SRAM window:** `sram_len` bytes from `sram_addr`.

## Output format (line-oriented for `diff`)

```
# SWD SNAPSHOT rdp=1 halted=1
REG.r0=0x40020000
...
REG.pc=0xFFFFFFFE
REG.xpsr=0x81000003
SCB.CFSR=0x00000000
SCB.HFSR=0x40000000
DBG.DHCSR=0x00030003
RCC.CR=0x03007183
FLASH.OPTCR=FAULT            # marked when the read NACKs (e.g. blocked at RDP1)
GPIO.A.MODER=0xA8000000
SRAM 0x20000000: DF F8 38 04 ...
...
```

## Fault handling (key for RDP1)

Every peripheral/memory read is wrapped: on ACK=0x4 print `<key>=FAULT`, call
`swd_clear_errors()` to recover the DP, and continue. So a snapshot never aborts
on a blocked flash-iface register — it records `FAULT` and moves on. This is what
makes the same command usable at RDP0 and RDP1 (OPTCR etc. simply show `FAULT`
when locked).

## Workflow

```
SWD SNAPSHOT > pre.txt         # (host redirects the capture)
<fire one glitch>
SWD SNAPSHOT > post.txt
diff pre.txt post.txt          # exactly what the fault flipped in non-flash state
```

## Implementation notes

- Reuse `swd_halt` / `swd_resume`, the `SWD REGS` core-register reader, and
  `swd_read_mem`; wrap reads with `swd_clear_errors()` recovery.
- Register `SNAPSHOT` in `swd_subcmds` (cli-errors) and add a config_none test
  (command recognised; graceful when no target).
- ~100 lines in `swd.c` + a dispatch branch; no new hardware. Pairs with a future
  `SWD WATCH` (DWT watchpoint) for triggered snapshots.

---

# TODO — flash-read leak probe (byte-at-a-time exfil hypothesis)

**Idea:** RDP1 blocks the *result* of a flash read (returns FAULT), but the flash
controller may still latch the real data somewhere observable before refusing it.
If any of it leaks into a register, SRAM, or a peripheral latch, we could pull
flash a byte at a time from a locked part — no glitch required.

**Why the unique pattern matters:** program flash so every location's value
encodes its own address (e.g. `word[A] = A`, or `byte[A] = f(A)` for byte
granularity). Then any leaked value immediately identifies *which* flash address
it came from — distinguishing a real leak from coincidence, and telling us the
leak's source/offset.

**Procedure:**
1. **RDP0:** program a unique address-encoding pattern across all of flash
   (`word[A]=A` is simplest; a byte-mixing function if we need byte-level ID).
   Verify with a normal read.
2. **Re-lock to RDP1** (pattern is retained; only a later unlock erases it).
3. **Baseline:** `SWD SNAPSHOT` with a wide SRAM window → `base.txt`.
4. **Provoke a locked flash read** of a chosen address A, via each path:
   - debug MEM-AP read of A (faults — but check what lands in the DP/AP data
     registers, RDBUFF, or a re-read afterwards);
   - bootloader Read-Memory of A (NACKs — snapshot after);
   - a CPU-side read from an SRAM stub at A (blocked — snapshot the stub's regs);
   - the same under a POR/voltage glitch on the read.
5. **Snapshot again** → `probe.txt`; `diff base.txt probe.txt`. Look for the
   address-encoded value of A (or its bytes) appearing in any REG.*, SRAM, or
   peripheral latch.
6. **If a leak channel is found:** sweep A across flash, reading the leaked
   byte(s) each time → reconstruct flash byte-by-byte.

**What would make it work / fail:** a hit means the controller exposes latched
read data (bus register, RDBUFF residue, DWT/ETM sample, or a peripheral) before
the block. A null result across all paths means the block is clean (data never
leaves the flash-controller boundary) — still a useful negative.

**Prereqs:** a wide-window `SWD SNAPSHOT` (done); a flash-fill-with-pattern helper
(`SWD FILL` writes SRAM/flash — extend to an address-encoding fill); RDP0→program→
RDP1 cycle. Complements the shadow/VCAP glitch work as a *non-glitch* avenue.

## Result (2026-09-29): NO LEAK — block is clean

Ran the experiment on the F401: programmed `0xCAFE0000..0xCAFE0007` (magic+index)
to flash @0x08000000 at RDP0, re-locked to RDP1 (pattern retained, flash reads
fault), then probed every accessible channel for the pattern.

| Channel | Observed | Leak? |
|---|---|---|
| CPU `ldr r1,[0x08000000]` (SRAM stub → bkpt) | HardFault; r1 stayed sentinel `0xDEADDEAD`; BFAR=0x08000000 (address only); CFSR precise bus fault | ✗ |
| Debug MEM-AP read (`SWD LEAKPROBE`, no clear) | DRW returned stale SRAM baseline; RDBUFF=0; STICKYERR=1 | ✗ |
| DP RDBUFF / AP DRW residue after fault | baseline/idle/fault — never `0xCAFE00xx` | ✗ |

**Conclusion:** the flash data value never leaves the flash-controller boundary.
Both the CPU-from-SRAM path and the debug MEM-AP path bus-fault cleanly; no flash
byte is latched into any register, RDBUFF, AP data reg, or fault register (BFAR
holds only the *address*). No byte-at-a-time exfil via a data-latch leak on this
part. `SWD LEAKPROBE <addr>` (added for this — atomic read + residue capture with
no intervening error-clear) confirms the auto-clear was not masking residue.

**Not yet closed:** behaviour *under a voltage glitch* during the read, and
whether ETM/DWT trace sampling of the faulting bus cycle exposes the data — both
are separate follow-ups.
