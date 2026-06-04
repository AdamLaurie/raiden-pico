# EFM32 Leopard Gecko — Debug-Lock Voltage Glitch

Target support for the **Silicon Labs EFM32LG** (Leopard Gecko, ARM Cortex-M3
@ 48 MHz). Bypasses the debug lock via voltage fault injection so protected flash
can be read over SWD. The EFM32 analogue of the nRF52 APPROTECT bypass and the
STM32 RDP1→RDP0 readout glitch.

Analysis: `WORK/efm32_findings.md` (FIND-EFM-001..006). Datasheet:
`WORK/efm32lg-datasheet.pdf`.

## Attack model

EFM32 protects debug with a single **Debug Lock Word (DLW)** in the flash
info-block lock-bits page. When locked:

- the SW-DP still connects (you can read DPIDR), but
- the **AHB-AP** (memory access) is **disabled** — flash/SRAM reads fault, and
- the only "official" recovery is the **Authentication Access Port (AAP)**
  `DEVICEERASE`, which mass-erases the part. The AAP can never *read*.

The DLW is latched by the MSC during the fixed **tRESET ≈ 163 µs** boot window.
A crowbar pulse on the **DECOUPLE** pin (the internal ~1.8 V core-LDO output)
during that window faults the DLW evaluation so the AHB-AP comes up **enabled** —
then flash is read over normal SWD, **without** triggering the AAP erase.

**Ground truth for "unlocked"** = the AHB-AP can actually read the factory
Device-Info (DI) PART word. A status bit is *not* trusted (same lesson as the
nRF FICR-read truth).

## Wiring

Shared SWD/crowbar rig with the nRF52840 module — only the inject node differs
(DECOUPLE instead of DEC1):

| Pico (RP2350) | EFM32LG | Notes |
|---|---|---|
| GP17 | DBG_SWCLK | SWD clock |
| GP18 | DBG_SWDIO | SWD data |
| GP15 | RESETn | active-low reset (for `sweeprst` / AAP) |
| GP2  | **DECOUPLE** | crowbar MOSFET drain → core rail (NOT main VDD) |
| GP22 | — | GLITCH_FIRED scope marker (trigger) |
| GP10/11/12 | target VDD switch | power-cycle control |
| GND  | GND | common ground |

- Crowbar = N-MOSFET, drain on DECOUPLE, source to GND, gate from GP2. Same part
  as the nRF DEC1 crowbar.
- **Remove or shrink the DECOUPLE decoupling cap** (1 µF stock) for a fast enough
  edge — a stiff cap absorbs the pulse.
- Run target VDD near the low end (~2.0 V) so the crowbar sinks less energy and
  the dip is sharper (FIND-EFM-004). BOD falling threshold is 1.74–1.96 V; keep
  the pulse sub-µs so a clean BOD reset doesn't just abort the attempt.

## Workflow

Firmware command surface (USB-CDC CLI on `/dev/ttyACM0`):

```
TARGET EFM32                         select the EFM32LG target
TARGET EFM INFO                      DPIDR + lock state + DI page (part/flash/uid)
TARGET EFM STATUS                    one-line DPIDR + UNLOCKED/LOCKED
TARGET EFM AAP                       scan AP IDRs for the AAP
TARGET EFM ERASE                     AAP DEVICEERASE recovery (DESTRUCTIVE)
TARGET EFM DUMP [addr] [bytes]       hex-dump memory over AHB-AP (needs unlock)
TARGET EFM SHOT [delay_us] [width]   one crowbar pulse, power-cycle (scope)
TARGET EFM SHOTRST [delay] [w] [hold] one crowbar pulse, nRST reboot (scope)
TARGET GLITCH DEBUGUNLOCK    [d0 d1 dstep w0 w1 wstep off settle tries]
TARGET GLITCH DEBUGUNLOCKRST [d0 d1 dstep w0 w1 wstep hold settle tries]
```

Host driver: `scripts/efm32_glitch.py` (lib `scripts/efm32_link.py`).

```bash
cd scripts

# 1. Confirm the link + that the part is actually LOCKED.
./efm32_glitch.py info

# 2. Scope bring-up: see the DECOUPLE droop before a long campaign.
#    Trigger the scope on GP22; probe DECOUPLE.
./efm32_glitch.py shot --delay 160 --width 100

# 3. Power-cycle sweep, centred on the tRESET ~163 us window. Wide+coarse first,
#    then bisect around any DISRUPT cluster (boot crashes localise the window).
./efm32_glitch.py sweep --d-start 100 --d-end 220 --d-step 1 \
                        --w-start 10 --w-end 200 --w-step 10 \
                        --dump-on-hit dump.bin --dump-size 0x10000

# 3b. Alternative: nRST-reboot sweep (VDD stays on, de-jittered boot, delay from
#     the reset release edge). Useful without a stiff bench PSU.
./efm32_glitch.py sweeprst --d-start 0 --d-end 200 --d-step 1

# 4. On unlock, read out flash.
./efm32_glitch.py dump --addr 0 --size 0x40000 --out flash.bin   # 256 kB part
```

Width is in 6.67 ns cycles (clamped firmware-side to ~67 ns..3.0 µs, 10..450 cyc).
Delay is µs: from power-on for `sweep` (centre ≈163 µs), from the nRST release edge
for `sweeprst` (tens of µs). Both sweeps pre-flight ABORT if the part is already
open, and stream `DISRUPT` lines (DPIDR lost = the glitch crashed the core) to help
localise the effective delay.

## AAP recovery (destructive)

`erase` is the **recovery** path, not the readout attack — it mass-erases the
device. Use it only to unbrick a part you own:

```bash
./efm32_glitch.py aap            # confirm the AAP is reachable (IDR 0x06E6xxxx)
./efm32_glitch.py erase --yes    # DEVICEERASE: flash wiped, debug re-opened
```

The AAP is only exposed briefly in the reset window, so `erase` connects under
reset and scans the AP IDRs for it.

## Caveats / to verify on the bench

- **DPIDR `0x2BA01477` is the generic Cortex-M3 SW-DP code** (shared by STM32F1,
  LPC17xx, …). It does *not* confirm an EFM32 — the DI PART read is the positive ID.
- **AAP register offsets** (CMD/CMDKEY/STATUS/IDR) and the **AAP IDR value**
  follow the OpenOCD `efm32` driver and the EFM32 RM. The AAP AP index is
  discovered at runtime by IDR rather than hard-coded. Confirm the IDR for the
  exact part with `TARGET EFM AAP` before trusting `erase`.
- **Bring-up discipline** (same as the nRF/PIC modules): validate `info`/`dump`
  against a known-UNLOCKED EFM32 first, so a parsing/wiring fault can't be
  mistaken for a glitch result.
- Exact **DLW word offset** in the lock-bits page (`0x0FE04000`) is not needed for
  the glitch (the AHB-AP read is the truth) — pull it from the EFM32LG Reference
  Manual if you want to script a targeted DLW probe.
