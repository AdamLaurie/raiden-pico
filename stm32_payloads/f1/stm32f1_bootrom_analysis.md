# STM32F1 Boot ROM Analysis (DEV_ID 0x410)

Dump source: 0x1FFFF000, 2048 bytes (system bootloader ROM)
Dumped from unlocked STM32F103 via SWD `READ 0x1FFFF000 512`

## Vector Table (0x1FFFF000)

| Offset | Value      | Purpose |
|--------|------------|---------|
| 0x000  | 0x200001FC | Initial SP (top of 512B SRAM used by bootloader) |
| 0x004  | 0x1FFFF021 | Reset vector (Thumb, -> 0x1FFFF020 trampoline) |
| 0x008  | 0x1FFFF765 | NMI handler |
| 0x00C  | 0x1FFFF767 | HardFault handler |

## Boot Entry (0x1FFFF010)

```
1FFFF010: ldr.w sp, [pc, #4]     ; SP = 0x200001FC
1FFFF014: bl    0x1FFFF76C       ; -> main bootloader init
```

Reset vector at 0x1FFFF004 points to trampoline at 0x1FFFF020:
```
1FFFF020: ldr r0, [pc, #0]      ; r0 = 0x1FFFF011 (Thumb address of boot entry)
1FFFF022: bx  r0                 ; jump to 0x1FFFF010
```

## Function Map

| Address    | Name            | Description |
|------------|-----------------|-------------|
| 0x1FFFF028 | flash_wait_bsy  | Poll FLASH_SR.BSY until clear |
| 0x1FFFF038 | iwdg_reload     | Write IWDG_KR reload key |
| 0x1FFFF040 | flash_bsy_wait  | Wait for FLASH_SR.BSY==0 |
| 0x1FFFF04A | flash_unlock_cr | Unlock FLASH_CR (KEYR sequence) |
| 0x1FFFF056 | flash_unlock_kr | Unlock FLASH_KEYR |
| 0x1FFFF062 | flash_full_unlock | Unlock both KEYR + CR + wait BSY |
| 0x1FFFF090 | opt_write       | Write all option bytes (R8=OBs, R10=flag) |
| 0x1FFFF11C | rdp_remove      | Write 0x00A5 to RDP option byte (disables RDP) |
| 0x1FFFF132 | rdp_check       | Read FLASH_OBR, test RDPRT bit |
| 0x1FFFF34C | main_init       | Main bootloader: IWDG, RCC, GPIO, USART setup |
| 0x1FFFF3FA | cmd_loop        | Command receive loop (get byte + complement) |
| 0x1FFFF416 | cmd_dispatch    | Switch on command byte |
| 0x1FFFF4AE | erase_gate      | Erase command RDP check (inverted!) |
| 0x1FFFF500 | write_mem_gate  | Write Memory RDP gate |
| 0x1FFFF518 | go_cmd          | GO command (0x21) handler |
| 0x1FFFF580 | user_jump       | `blx r5` — jump to user-supplied address |
| 0x1FFFF58A | read_mem        | Read Memory (0x11) handler |
| 0x1FFFF59C | read_rdp_gate   | `beq` rejects read if RDP active |
| 0x1FFFF678 | opt_rdp_gate    | Gate before option byte write commands |
| 0x1FFFF76C | boot_main       | Called from reset — full bootloader entry |

## Literal Pool (decoded from 0x1FFFF424+)

| Pool Addr  | Value      | Meaning |
|------------|------------|---------|
| 0x1FFFF428 | 0x40022000 | FLASH_ACR base (FLASH controller) |
| 0x1FFFF42C | 0x0000AAAA | IWDG_KR reload value |
| 0x1FFFF430 | 0x40003000 | IWDG base address |
| 0x1FFFF434 | 0x40022000 | FLASH register base |
| 0x1FFFF438 | 0x45670123 | FLASH_KEYR key 1 |
| 0x1FFFF43C | 0xCDEF89AB | FLASH_KEYR key 2 |
| 0x1FFFF440 | 0x1FFFF800 | Option bytes base (OB area in flash) |

## RDP Check Flow (Critical Path)

### 1. RDP Status Read (0x1FFFF132)
```
1FFFF132: ldr  r0, [pc, ...]   ; r0 = 0x40022000 (FLASH base)
1FFFF134: ldr  r0, [r0, #28]   ; r0 = FLASH_OBR (offset 0x1C)
1FFFF136: lsls r0, r0, #30     ; shift bit 1 (RDPRT) to N flag
1FFFF138: ...                   ; (may have intermediate instruction)
1FFFF13A: bpl  <rdp_not_set>   ; N=0 means RDP not active -> skip protection
```

**FLASH_OBR (0x4002201C):**
- Bit 1 = RDPRT: Read protection active
- `lsls r0, r0, #30` shifts bit 1 into the sign bit (bit 31)
- If RDPRT=1: N flag set -> `bpl` NOT taken -> RDP enforced
- If RDPRT=0: N flag clear -> `bpl` taken -> unprotected

### 2. RDP Branch (0x1FFFF13A) — TOP GLITCH TARGET
This is the critical branch. A voltage glitch during this `bpl` instruction
can flip the branch outcome, making the bootloader believe RDP is not set.

**Glitch strategy:**
- Glitch must corrupt the instruction fetch or flag evaluation
- Target: `bpl` at 0x1FFFF13A
- Window: single instruction (~125ns at 8MHz)
- Use DWT_CYCCNT to measure exact cycle count from reset

### 3. Per-Command RDP Gates
Each sensitive command re-checks an RDP flag (stored in a register or RAM):

| Gate Address | Command | Branch | Effect |
|-------------|---------|--------|--------|
| 0x1FFFF59C  | Read Memory (0x11)  | `beq` skip if RDP=1 | Blocks flash readout |
| 0x1FFFF500  | Write Memory (0x31) | `beq` skip if RDP=1 | Blocks memory writes |
| 0x1FFFF4AE  | Erase (0x43/0x44)   | inverted logic       | Only allows erase when RDP=1 (mass erase to unprotect) |
| 0x1FFFF678  | Option bytes        | gate check           | Blocks option byte modification |

## Boot Flow: Reset -> RDP Check

1. **Reset** (0x1FFFF010): Load SP, branch to main init
2. **main_init** (0x1FFFF34C): Configure IWDG, RCC (8MHz HSI), GPIO, USART
3. **rdp_check** (0x1FFFF132): Read FLASH_OBR.RDPRT, set internal flag
4. **cmd_loop** (0x1FFFF3FA): Wait for USART command byte
5. **cmd_dispatch** (0x1FFFF416): Route to handler based on command
6. Each handler checks RDP flag before executing

## Attack Chains

### Chain 1: Boot-Time RDP Bypass (voltage glitch)
- Target: `bpl` at 0x1FFFF13A
- Method: Power glitch during boot to corrupt RDP check
- Result: Bootloader thinks RDP=0, all commands unlocked
- Timing: ~48K cycles from nRST release (measure with TIMING command)

### Chain 2: Per-Command Gate Bypass
- Target: Individual `beq` gates (0x1FFFF59C for read, etc.)
- Method: Glitch during specific command processing
- Harder: Must time glitch to command processing, not just boot

### Chain 3: FPB Redirect (no glitch needed if debug access available)
- Use Flash Patch Breakpoint unit to redirect reset vector
- SRAM payload reconfigures system before bootloader runs
- See `rdp_bypass.S` for implementation

### Chain 4: Option Byte Direct Write
- Target: 0x1FFFF090 (opt_write) or 0x1FFFF11C (rdp_remove)
- If RDP gate bypassed, can write 0xA5 to option bytes to disable RDP
- Triggers mass erase but gives debug access back
