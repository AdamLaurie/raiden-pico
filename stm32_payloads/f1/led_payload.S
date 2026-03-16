/*
 * Minimal STM32F103 SRAM payload — blinks PA5 (LD2 on Nucleo-F103RB)
 * Blink rate indicates debug domain state:
 *   FAST blink (~100ms) = C_DEBUGEN clear (debug domain POR'd)
 *   SLOW blink (~500ms) = C_DEBUGEN set (debug domain survived)
 *
 * On STM32F103, SRAM boot entry is NOT at offset 0x08 — the core uses a
 * hardcoded vector table size that places the entry at one of:
 *   0x20000108, 0x200001CC, or 0x200001E0
 * depending on the number of interrupt vectors.
 *
 * We use a NOP sled from the vector table through 0x200001E0 to catch all
 * possible entry points, then fall through to the actual payload code.
 */
.syntax unified
.thumb
.cpu cortex-m3

.section .text

/* Vector table */
.word 0x20005000          /* Initial SP (top of 20KB SRAM) */
.word nop_sled + 1        /* Reset vector -> start of NOP sled */

/* NOP sled: covers 0x20000008 through 0x200001FF (252 NOPs = 504 bytes) */
nop_sled:
.rept 252
    nop
.endr

/* Actual payload starts here at ~0x20000200 — all entry points fall through */
payload_start:
    /* Enable GPIOA clock: RCC_APB2ENR |= (1 << 2) */
    ldr r0, =0x40021018   /* RCC_APB2ENR */
    ldr r1, [r0]
    orrs r1, r1, #4       /* IOPAEN = bit 2 */
    str r1, [r0]

    /* Small delay for clock to stabilize */
    movs r2, #100
1:  subs r2, #1
    bne 1b

    /* Configure PA5 as push-pull output, 2MHz */
    ldr r0, =0x40010800   /* GPIOA_CRL */
    ldr r1, [r0]
    bic r1, r1, #0x00F00000
    orr r1, r1, #0x00200000
    str r1, [r0]

    /* Read DHCSR to check C_DEBUGEN (bit 0) */
    ldr r0, =0xE000EDF0   /* DHCSR */
    ldr r1, [r0]
    tst r1, #1             /* C_DEBUGEN = bit 0 */

    /* Select delay count based on debug state */
    ite eq
    ldreq r5, =80000       /* C_DEBUGEN=0: fast blink (~100ms at 8MHz) */
    ldrne r5, =400000      /* C_DEBUGEN=1: slow blink (~500ms at 8MHz) */

    /* Blink loop using BSRR for atomic set/reset */
    ldr r4, =0x40010810   /* GPIOA_BSRR */

blink:
    /* Set PA5 high */
    movs r0, #32
    str r0, [r4]
    mov r2, r5
2:  subs r2, #1
    bne 2b

    /* Reset PA5 low: BSRR bit 21 */
    mov r0, #(1 << 21)
    str r0, [r4]
    mov r2, r5
3:  subs r2, #1
    bne 3b

    b blink

.pool
