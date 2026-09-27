/*
 * STM32F103 RDP1 "clean wake" control test
 *
 * Launched by nRST into SRAM-boot (BOOT0=1, BOOT1=1) with NO debugger attached
 * (SWD tri-stated before the reset). C_DEBUGEN is 0 by reset default — the CPU
 * has never been debugged in this run. The payload:
 *   1. Inits USART1 (PA9, 115200 @ 8MHz HSI) and sends "CLN0"
 *   2. Enters STOP mode, wakes ~2ms later via RTC alarm (autonomous, timed)
 *   3. Re-inits USART, sends "WAKE"
 *   4. Sends "OBR:" + FLASH_OBR (RDPRT status, 4B LE)
 *   5. Sends "DATA" + 64 words (256 B) read DIRECTLY from flash 0x08000000
 *   6. Sends "DONE", or "FAULT\n" if a read bus-faults
 *
 * Deliberately does NOT touch DHCSR/DEMCR/DBGMCU/FPB — this is the pure test of
 * "does a genuinely non-debugged, self-timed wake let SRAM code read flash under
 * RDP1?".  Result at RDP1: FAULT (RDP is the flash-controller POR latch, RDPRT=1).
 *
 * RDP0 Phase A (rdp-payload-check skill) PASS — 2026-09-27, F1 unlocked to RDP0
 * with marker 0xCAFEBABE @ 0x08000000: payload returned "DATA" + BE BA FE CA +
 * FF... + "DONE" (OBR RDPRT=0). Read path proven good, so the RDP1 FAULT is RDP
 * enforcement, not a payload bug.
 */
.syntax unified
.thumb
.cpu cortex-m3

.section .text

/* ========== Vector table (SRAM boot: SP@0x20000000, reset@0x20000004) ========== */
.word 0x20005000          /* Initial SP (top of 20KB SRAM) */
.word entry + 1           /* Reset vector */

.align 2
entry:
    ldr sp, =0x20005000

    /* Enable GPIOA + USART1 clocks: RCC_APB2ENR |= IOPAEN | USART1EN */
    ldr r0, =0x40021018    /* RCC_APB2ENR */
    ldr r1, [r0]
    orrs r1, r1, #4        /* IOPAEN */
    orr r1, r1, #0x4000    /* USART1EN */
    str r1, [r0]

    movs r2, #100
1:  subs r2, #1
    bne 1b

    /* PA9 = AF push-pull (GPIOA_CRH nibble) */
    ldr r0, =0x40010804    /* GPIOA_CRH */
    ldr r1, [r0]
    bic r1, r1, #0xF0
    orr r1, r1, #0xA0
    str r1, [r0]

    /* USART1: 115200 @ 8MHz HSI */
    ldr r0, =0x40013808    /* USART1_BRR */
    movs r1, #0x45
    str r1, [r0]
    ldr r0, =0x4001380C    /* USART1_CR1 */
    movw r1, #0x2008       /* UE + TE */
    str r1, [r0]
    movs r2, #200
2:  subs r2, #1
    bne 2b

    ldr r4, =0x40013800    /* USART1 base */

    /* Send "CLN0" — payload is running, pre-sleep */
    movs r0, #0x43         /* 'C' */
    bl uart_send_byte
    movs r0, #0x4C         /* 'L' */
    bl uart_send_byte
    movs r0, #0x4E         /* 'N' */
    bl uart_send_byte
    movs r0, #0x30         /* '0' */
    bl uart_send_byte

    /* ---------------- Enter STOP with RTC alarm wake (~2ms) ---------------- */

    /* Enable PWR + BKP clocks: RCC_APB1ENR |= (1<<28)|(1<<27) */
    ldr r0, =0x4002101C    /* RCC_APB1ENR */
    ldr r1, [r0]
    orr r1, r1, #0x18000000
    str r1, [r0]
    movs r2, #10
3:  subs r2, #1
    bne 3b

    /* Backup domain write access: PWR_CR |= DBP (1<<8) */
    ldr r0, =0x40007000    /* PWR_CR */
    ldr r1, [r0]
    orr r1, r1, #0x100
    str r1, [r0]

    /* Enable LSI: RCC_CSR |= 1, wait LSIRDY (bit1) */
    ldr r0, =0x40021024    /* RCC_CSR */
    ldr r1, [r0]
    orr r1, r1, #1
    str r1, [r0]
4:  ldr r1, [r0]
    tst r1, #2
    beq 4b

    /* RTCSEL = LSI, RTCEN */
    ldr r0, =0x40021020    /* RCC_BDCR */
    ldr r1, [r0]
    bic r1, r1, #0x300
    orr r1, r1, #0x200
    str r1, [r0]
    ldr r1, [r0]
    orr r1, r1, #0x8000
    str r1, [r0]

    /* Wait RTC sync (RSF bit3 in RTC_CRL) */
    ldr r0, =0x40002804    /* RTC_CRL */
5:  ldr r1, [r0]
    tst r1, #8
    beq 5b

    /* Config mode: wait RTOFF (bit5), set CNF (bit4) */
6:  ldr r1, [r0]
    tst r1, #0x20
    beq 6b
    ldr r1, [r0]
    orr r1, r1, #0x10
    str r1, [r0]

    /* No prescaler */
    ldr r0, =0x40002808    /* RTC_PRLH */
    movs r1, #0
    str r1, [r0]
    ldr r0, =0x4000280C    /* RTC_PRLL */
    str r1, [r0]

    /* Alarm = CNT + 80 (~2ms at ~40kHz) */
    ldr r0, =0x40002818    /* RTC_CNTH */
    ldr r1, [r0]
    ldr r0, =0x4000281C    /* RTC_CNTL */
    ldr r2, [r0]
    orr r2, r2, r1, lsl #16
    adds r2, r2, #80
    ldr r0, =0x40002820    /* RTC_ALRH */
    lsrs r1, r2, #16
    str r1, [r0]
    ldr r0, =0x40002824    /* RTC_ALRL */
    uxth r1, r2
    str r1, [r0]

    /* Exit config: clear CNF, wait RTOFF */
    ldr r0, =0x40002804    /* RTC_CRL */
    ldr r1, [r0]
    bic r1, r1, #0x10
    str r1, [r0]
7:  ldr r1, [r0]
    tst r1, #0x20
    beq 7b

    /* Alarm interrupt: RTC_CRH |= ALRIE (1<<1) */
    ldr r0, =0x40002800    /* RTC_CRH */
    ldr r1, [r0]
    orr r1, r1, #2
    str r1, [r0]

    /* EXTI line 17 (RTC alarm): IMR/EMR/RTSR */
    ldr r0, =0x40010400    /* EXTI_IMR */
    ldr r1, [r0]
    orr r1, r1, #0x20000
    str r1, [r0]
    ldr r0, =0x40010404    /* EXTI_EMR */
    ldr r1, [r0]
    orr r1, r1, #0x20000
    str r1, [r0]
    ldr r0, =0x40010408    /* EXTI_RTSR */
    ldr r1, [r0]
    orr r1, r1, #0x20000
    str r1, [r0]

    /* PWR_CR.PDDS = 0 (STOP not STANDBY) */
    ldr r0, =0x40007000    /* PWR_CR */
    ldr r1, [r0]
    bic r1, r1, #2
    str r1, [r0]

    /* SCB_SCR |= SEVONPEND | SLEEPDEEP */
    ldr r0, =0xE000ED10    /* SCB_SCR */
    ldr r1, [r0]
    orr r1, r1, #0x14
    str r1, [r0]

    /* Sleep */
    dsb
    wfe

    /* ---------------- Woken from STOP ---------------- */
    ldr r0, =0xE000ED10    /* SCB_SCR */
    ldr r1, [r0]
    bic r1, r1, #0x14
    str r1, [r0]

    /* STOP gated clocks — re-enable GPIOA + USART1 */
    ldr r0, =0x40021018    /* RCC_APB2ENR */
    ldr r1, [r0]
    orr r1, r1, #4
    orr r1, r1, #0x4000
    str r1, [r0]
    ldr r2, =5000
8:  subs r2, #1
    bne 8b

    ldr r0, =0x40010804    /* GPIOA_CRH: PA9 AF PP */
    ldr r1, [r0]
    bic r1, r1, #0xF0
    orr r1, r1, #0xA0
    str r1, [r0]

    ldr r0, =0x40013808    /* USART1_BRR */
    movs r1, #0x45
    str r1, [r0]
    ldr r0, =0x4001380C    /* USART1_CR1 */
    movw r1, #0x2008
    str r1, [r0]
    movs r2, #200
9:  subs r2, #1
    bne 9b

    ldr r4, =0x40013800    /* USART1 base */

    /* Send "WAKE" */
    movs r0, #0x57         /* 'W' */
    bl uart_send_byte
    movs r0, #0x41         /* 'A' */
    bl uart_send_byte
    movs r0, #0x4B         /* 'K' */
    bl uart_send_byte
    movs r0, #0x45         /* 'E' */
    bl uart_send_byte

    /* Send "DHC:" + DHCSR — did the STOP/wake clear C_DEBUGEN by itself? */
    movs r0, #0x44         /* 'D' */
    bl uart_send_byte
    movs r0, #0x48         /* 'H' */
    bl uart_send_byte
    movs r0, #0x43         /* 'C' */
    bl uart_send_byte
    movs r0, #0x3A         /* ':' */
    bl uart_send_byte
    ldr r5, =0xE000EDF0    /* DHCSR */
    ldr r3, [r5]
    bl send_word

    /* Send "OBR:" + FLASH_OBR (RDPRT status) */
    movs r0, #0x4F         /* 'O' */
    bl uart_send_byte
    movs r0, #0x42         /* 'B' */
    bl uart_send_byte
    movs r0, #0x52         /* 'R' */
    bl uart_send_byte
    movs r0, #0x3A         /* ':' */
    bl uart_send_byte
    ldr r5, =0x4002201C    /* FLASH_OBR */
    ldr r3, [r5]
    bl send_word

    /* Install a HardFault handler so a blocked read reports "FAULT" not a hang */
    ldr r6, =0x20004F00
    ldr r0, =0x20005000
    str r0, [r6, #0]          /* SP */
    ldr r0, =hardfault_handler + 1
    str r0, [r6, #0x0C]       /* HardFault vector */
    ldr r0, =0xE000ED08       /* VTOR */
    str r6, [r0]

    /* Send "DATA" then dump 64 words (256 B) DIRECT from flash */
    movs r0, #0x44         /* 'D' */
    bl uart_send_byte
    movs r0, #0x41         /* 'A' */
    bl uart_send_byte
    movs r0, #0x54         /* 'T' */
    bl uart_send_byte
    movs r0, #0x41         /* 'A' */
    bl uart_send_byte

    ldr r5, =0x08000000    /* Flash base */
    movs r7, #64           /* word count */
dump_loop:
    ldr r3, [r5]           /* direct 32-bit flash read (bus-faults if RDP blocks) */
    bl send_word
    adds r5, #4
    subs r7, #1
    bne dump_loop

    /* Send "DONE" and spin */
    movs r0, #0x44         /* 'D' */
    bl uart_send_byte
    movs r0, #0x4F         /* 'O' */
    bl uart_send_byte
    movs r0, #0x4E         /* 'N' */
    bl uart_send_byte
    movs r0, #0x45         /* 'E' */
    bl uart_send_byte
done_spin:
    b done_spin

/* HardFault handler: send "FAULT\n" then spin */
hardfault_handler:
    ldr r4, =0x40013800
    movs r0, #0x46         /* 'F' */
    bl uart_send_byte
    movs r0, #0x41         /* 'A' */
    bl uart_send_byte
    movs r0, #0x55         /* 'U' */
    bl uart_send_byte
    movs r0, #0x4C         /* 'L' */
    bl uart_send_byte
    movs r0, #0x54         /* 'T' */
    bl uart_send_byte
    movs r0, #0x0A         /* '\n' */
    bl uart_send_byte
1:  b 1b

/* Send word in r3 (4 bytes LE). Clobbers r0-r3 */
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

uart_send_byte:
    and r0, r0, #0xFF
1:  ldr r1, [r4]          /* USART1_SR */
    tst r1, #0x80          /* TXE */
    beq 1b
    str r0, [r4, #4]      /* USART1_DR */
    bx lr

.pool
