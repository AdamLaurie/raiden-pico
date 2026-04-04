/*
 * STM32F103 Boot ROM Trace Payload — exact instruction replica
 *
 * Replicates the bootloader's exact instruction sequence in SRAM
 * where DWT PC-match breakpoints work.
 *
 * Flow (identical to real bootloader):
 *   init → USART1 setup → send initial 0x79 → cmd_loop
 *   cmd_loop: recv cmd+complement → validate → dispatch cascade
 *   Read Memory (0x11): bl get_addr_with_rdp → rdp_check → ACK 0x79 → BKPT
 *
 * Usage:
 *   1. Load to SRAM via SWD WRITE MEM
 *   2. Set DWT PC-match on rdp_check label
 *   3. TRIGGER UART 79, ARM TRACE
 *   4. Set PC/SP, resume
 *   5. Send 0x11 0xEE via TARGET SEND (Read Memory cmd + complement)
 *   6. Trigger fires on ACK 0x79, trace captures power
 *
 * The RDP check happens BEFORE the ACK — use pre-trigger buffer to capture it.
 */
.syntax unified
.thumb
.cpu cortex-m3

.section .text

/* ========== Entry point (loaded to SRAM, PC set by SWD) ========== */
.global _start
_start:
    /* Disable interrupts — bootloader 0x1FFFF7A4 */
    cpsid i

    /* === IWDG setup — bootloader 0x1FFFF35E-0x1FFFF36E === */
    ldr r2, =0x40003000     /* IWDG base */
    movw r0, #0x5555
    str r0, [r2]            /* IWDG_KR = 0x5555 */
    movs r0, #6
    str r0, [r2, #4]        /* IWDG_PR = 6 */
    movw r3, #0xAAAA
    str r3, [r2]            /* IWDG_KR = 0xAAAA */

    /* === RCC setup — bootloader 0x1FFFF370-0x1FFFF3CE === */
    ldr r0, =0x40021000     /* RCC base */
    mov.w r1, #0x100000
    str r1, [r0, #4]        /* RCC_CFGR */
    ldr r1, [r0]
    orr r1, r1, #1
    str r1, [r0]             /* HSION */
1:  ldr r1, [r0]
    tst r1, #2
    beq 1b                   /* Wait HSIRDY */

    /* NVIC_VTOR = 0 — bootloader 0x1FFFF396 */
    mov.w r6, #0xE000E000
    movs r4, #0
    str r4, [r6, #0xD08]

    /* === USART1 GPIO + clock — bootloader 0x1FFFF38E-0x1FFFF3F0 === */
    ldr r0, =0x40021018     /* RCC_APB2ENR */
    ldr r1, [r0]
    orr r1, r1, #4          /* IOPAEN */
    orr r1, r1, #0x4000     /* USART1EN */
    str r1, [r0]

    movs r2, #100
2:  subs r2, #1
    bne 2b

    /* PA9: AF push-pull 2MHz */
    ldr r0, =0x40010804     /* GPIOA_CRH */
    ldr r1, [r0]
    bic r1, r1, #0xF0
    orr r1, r1, #0xA0
    str r1, [r0]

    /* PA10: floating input */
    ldr r1, [r0]
    bic r1, r1, #0xF00
    orr r1, r1, #0x400
    str r1, [r0]

    /* USART1: 115200 @ 8MHz HSI — bootloader 0x1FFFF3D0-0x1FFFF3F0 */
    ldr r0, =0x40013800     /* USART1 base */
    movs r1, #0x45
    str r1, [r0, #8]        /* BRR = 0x45 */
    movw r1, #0x340C         /* UE + M(9-bit) + PCE(even parity) + TE + RE = 8E1 */
    str r1, [r0, #0xC]      /* CR1 — matches real bootloader 0x1FFFF3EC */

    movs r2, #200
3:  subs r2, #1
    bne 3b

    /* Drain stale USART1 RX data — not in real bootloader but needed
     * because SWD halt/resume can leave bytes in the FIFO */
    ldr r2, =0x40013800     /* USART1 base */
4:  ldrh r1, [r2]           /* USART1_SR */
    tst r1, #0x20           /* RXNE? */
    beq 5f
    ldrh r1, [r2, #4]       /* Read DR to clear RXNE */
    b 4b
5:

    /* === Send initial 0x79 — bootloader 0x1FFFF3F2 === */
    movs r0, #0x79
    bl uart_send_byte

    /* r7 = AIRCR value for sys reset — bootloader 0x1FFFF3F8 */
    ldr r7, =0x05FA0004

    /* ================================================================
     * cmd_loop — exact replica of bootloader 0x1FFFF3FA
     * Recv cmd byte + complement, validate XOR, dispatch
     * ================================================================ */
.global cmd_loop
cmd_loop:
    /* Kick IWDG — bootloader 0x1FFFF3FA-0x1FFFF400 */
    ldr r0, =0x40003000     /* IWDG base */
    movw r1, #0xAAAA
    str r1, [r0]             /* IWDG_KR reload */

    /* Recv command byte — bootloader 0x1FFFF402 */
    bl uart_recv_byte
    mov r4, r0               /* r4 = cmd */

    /* Recv complement — bootloader 0x1FFFF408 */
    bl uart_recv_byte

    /* Validate: cmd XOR complement == 0xFF — bootloader 0x1FFFF40C */
    eor.w r1, r0, r4
    cmp r1, #0xFF
    beq cmd_dispatch
    movs r4, #0x55           /* Force invalid cmd — bootloader 0x1FFFF414 */

    /* ================================================================
     * cmd_dispatch — exact cascade from bootloader 0x1FFFF416
     * Same comparison order as real bootloader binary tree
     * ================================================================ */
cmd_dispatch:
    cmp r4, #0x31            /* Write Unprotect */
    beq cmd_nack_loop
    bgt dispatch_upper
    cmp r4, #0x02            /* Get ID */
    beq cmd_nack_loop
    bgt dispatch_mid
    cbz r4, cmd_nack_loop    /* Get (0x00) */
    cmp r4, #0x01            /* Get Version (0x01) */
    bne cmd_nack
    b cmd_nack_loop

dispatch_mid:
    /* r4 > 0x02: check 0x11, 0x21 */
    cmp r4, #0x11            /* Read Memory */
    beq cmd_read_mem
    cmp r4, #0x21            /* Go */
    beq cmd_nack_loop
    b cmd_nack

dispatch_upper:
    /* r4 > 0x31: check 0x73, 0x43, 0x63, etc. */
    cmp r4, #0x73            /* Write Protect */
    beq cmd_nack_loop
    bgt dispatch_upper2
    cmp r4, #0x43            /* Erase */
    beq cmd_nack_loop
    cmp r4, #0x63            /* Readout Unprotect */
    beq cmd_nack_loop
    b cmd_nack

dispatch_upper2:
    cmp r4, #0x82            /* Write Memory Extension */
    beq cmd_nack_loop
    cmp r4, #0x92            /* Extended Erase */
    beq cmd_nack_loop
    b cmd_nack

    /* Commands we don't handle — NACK and loop */
cmd_nack:
    movs r0, #0x1F
    bl uart_send_byte
cmd_nack_loop:
    b cmd_loop

    /* ================================================================
     * Read Memory (0x11) — bootloader 0x1FFFF4F4
     * Calls get_addr_with_rdp which does: rdp_check → ACK → recv addr
     * We only need: rdp_check → ACK → BKPT
     * ================================================================ */
cmd_read_mem:
    bl get_addr_with_rdp

    /* After get_addr_with_rdp returns, halt for measurement */
    bkpt #0

    /* Loop back for repeated captures */
    b cmd_loop

    /* ================================================================
     * get_addr_with_rdp — exact replica of bootloader 0x1FFFF2F8
     * push → rdp_check → cbnz (NACK if RDP) → ACK 0x79 → BKPT
     * ================================================================ */
get_addr_with_rdp:
    push {r4, r5, lr}
    movs r5, #0

    /* MARKER: 8x 0xAA burst before rdp_check — wide dip in power trace */
    movs r4, #8
2:  movs r0, #0xAA
    bl uart_send_byte
    subs r4, #1
    bne 2b

    /* rdp_check — bootloader 0x1FFFF2FC */
    bl rdp_check

    /* cbnz r0, nack — bootloader 0x1FFFF300 */
    cbnz r0, get_addr_nack

    /* ACK — bootloader 0x1FFFF302-0x1FFFF304 */
    /* THIS is where TRIGGER UART 0x79 fires */
    movs r0, #0x79
    bl uart_send_byte

    /* In real bootloader, would recv 4 addr bytes + checksum here.
     * We return immediately — the ACK is all we need for the trace. */
    pop {r4, r5, pc}

get_addr_nack:
    movs r0, #0x1F
    bl uart_send_byte
    mov.w r0, #0x55555555
    pop {r4, r5, pc}

    /* ================================================================
     * rdp_check — exact replica of bootloader 0x1FFFF132
     * Bracketed by 0xAA/0xBB UART markers in caller for trace visibility
     * ================================================================ */
.global rdp_check
rdp_check:
    ldr r1, =0x40022000     /* FLASH register base */
    movs r0, #0
    ldr r1, [r1, #0x1C]     /* FLASH_OBR */
    lsls r1, r1, #30        /* Test bit 1 (RDPRT) */
    bpl rdp_not_set
    movs r0, #1
rdp_not_set:
    bx lr

/* ================================================================
 * UART helpers — exact replicas of bootloader functions
 * ================================================================ */

/* uart_send_byte — bootloader 0x1FFFF028
 * Write byte to USART1_DR, wait for TXE
 * Uses literal pool for USART1 base (same as bootloader) */
uart_send_byte:
    ldr r1, =0x40013800     /* USART1 base */
    strh r0, [r1, #4]       /* USART1_DR = data */
1:  ldrh r0, [r1]           /* USART1_SR */
    lsls r0, r0, #25        /* Test bit 6 (TC) */
    bpl 1b                   /* Wait for TC */
    bx lr

/* uart_recv_byte — bootloader 0x1FFFF1E2
 * Wait for RXNE, kick IWDG while waiting, return byte in r0 */
uart_recv_byte:
    ldr r2, =0x40013800     /* USART1 base */
    movw r0, #0xAAAA
    ldr r1, =0x40003000     /* IWDG base */
1:  str r0, [r1]             /* IWDG_KR = 0xAAAA (kick) */
    ldrh r3, [r2]            /* USART1_SR */
    lsls r3, r3, #26        /* Test bit 5 (RXNE) */
    bpl 1b                   /* Loop until RXNE */
    ldrh r0, [r2, #4]       /* USART1_DR */
    uxtb r0, r0
    bx lr

.pool
