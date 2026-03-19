/*
 * STM32F103 RDP1 — Reset/Power Mode Disruption Test
 *
 * Tries various internal reset and low-power sequences to see if any
 * can disrupt the flash controller's RDP latch without voltage glitch.
 *
 * Uses BKP_DR1 as phase counter (survives all resets except power loss).
 * FPB survives system resets (IWDG, WWDG, NRST, SW) so stage 2 re-runs.
 *
 * Phase 0: Baseline flash read + rapid STOP/wake cycling (LPDS=1) + re-test
 * Phase 1: Post-IWDG reset — test flash
 * Phase 2: Post-WWDG reset — test flash
 * Phase 3: Post-STANDBY wakeup — test flash (if FPB survived)
 * Phase 4: Done, clear BKP
 *
 * Protocol: "RST1" header + phase(1B) + flash_test_result + "NEXT"/"DONE"
 *   Flash test: 'O' + data(4B) = success, 'F' + PC(4B) = fault
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

/* ========== STAGE 1: STOP debug kill ========== */
stage1_start:   /* 0x20000200 */
    /* Enable GPIOA + PWR + BKP clocks */
    ldr r0, =0x40021018    /* RCC_APB2ENR */
    ldr r1, [r0]
    orrs r1, r1, #4
    str r1, [r0]
    ldr r0, =0x4002101C    /* RCC_APB1ENR */
    ldr r1, [r0]
    orr r1, r1, #0x18000000  /* PWR + BKP */
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

    /* Configure FPB — COMP0 for reset vector redirect */
    ldr r0, =0xE0002000    /* FPB base */
    movs r1, #3             /* FP_CTRL: ENABLE + KEY */
    str r1, [r0, #0]
    movs r1, #0x20          /* FP_REMAP: base 0x20000020 */
    str r1, [r0, #4]
    movs r1, #0x05          /* COMP0: addr 0x04, ENABLE */
    str r1, [r0, #8]

    /* ---- Aggressively clear debug state ---- */
    ldr r0, =0xE000EDF0
    ldr r1, =0xA05F0000
    str r1, [r0]
    ldr r0, =0xE000EDFC
    movs r1, #0
    str r1, [r0]
    ldr r0, =0xE0042004
    movs r1, #0
    str r1, [r0]
    ldr r0, =0xE0001000
    movs r1, #0
    str r1, [r0]
    ldr r0, =0xE0000FB0    /* ITM_LAR */
    movs r1, #0
    str r1, [r0]
    ldr r0, =0xE0001FB0    /* DWT_LAR */
    str r1, [r0]
    ldr r0, =0xE0040FB0    /* TPIU_LAR */
    str r1, [r0]

    /* ---- Enter STOP mode ---- */
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
    /* Enable backup domain write access first */
    ldr r0, =0x40007000    /* PWR_CR */
    ldr r1, [r0]
    orr r1, r1, #0x100     /* DBP */
    str r1, [r0]

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
    tst r1, #0x20           /* RTOFF */
    beq 5b
    ldr r1, [r0]
    orr r1, r1, #0x10       /* CNF */
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


/* ========== STAGE 2: Reset/Power Mode Disruption Tests ========== */
.align 2
stage2_start:
    ldr sp, =0x20005000

    /* Re-enable all needed clocks:
     * APB2: GPIOA(2), USART1(14), AFIO(0)
     * APB1: PWR(28), BKP(27)
     */
    ldr r0, =0x40021018    /* RCC_APB2ENR */
    ldr r1, [r0]
    orr r1, r1, #0x4000    /* USART1 */
    orr r1, r1, #0x05      /* GPIOA + AFIO */
    str r1, [r0]
    ldr r0, =0x4002101C    /* RCC_APB1ENR */
    ldr r1, [r0]
    orr r1, r1, #0x18000000  /* PWR + BKP */
    str r1, [r0]
    ldr r2, =50000
1:  subs r2, #1
    bne 1b

    /* PA5 LED on */
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

    ldr r4, =0x40013800    /* USART1 base — keep in r4 */

    /* Set up vector table in SRAM */
    ldr r6, =hardfault_vector_table
    ldr r0, =0x20005000
    str r0, [r6, #0]           /* SP */
    ldr r0, =hardfault_handler + 1
    str r0, [r6, #0x0C]        /* HardFault vector (exception 3) */
    /* RTC alarm = IRQ 41, vector offset = (41+16)*4 = 0xE4 */
    ldr r0, =rtc_alarm_isr + 1
    str r0, [r6, #0xE4]
    ldr r0, =0xE000ED08
    str r6, [r0]                /* VTOR = SRAM table */

    /* Enable backup domain write access */
    ldr r0, =0x40007000    /* PWR_CR */
    ldr r1, [r0]
    orr r1, r1, #0x100     /* DBP */
    str r1, [r0]

    /* Read phase from BKP_DR1 (0x40006C04) */
    ldr r8, =0x40006C04    /* BKP_DR1 */
    ldr r9, [r8]
    and r9, r9, #0xFF       /* phase = low byte */

    /* --- Send header "RST1" + phase byte --- */
    movs r0, #0x52     /* 'R' */
    bl uart_send_byte
    movs r0, #0x53     /* 'S' */
    bl uart_send_byte
    movs r0, #0x54     /* 'T' */
    bl uart_send_byte
    movs r0, #0x31     /* '1' */
    bl uart_send_byte
    mov r0, r9
    bl uart_send_byte

    /* --- Read RCC_CSR for reset source --- */
    ldr r5, =0x40021024
    ldr r3, [r5]
    bl send_word

    /* --- Flash read test --- */
    ldr r7, =test_state
    movs r0, #1             /* Assume fault */
    str r0, [r7]
    ldr r5, =0x08000000
    ldr r3, [r5]            /* May fault — handler skips this */
    movs r0, #0             /* No fault */
    str r0, [r7]

    /* Check if we faulted */
    ldr r0, [r7]
    cmp r0, #0
    beq flash_ok
    /* Fault path */
    movs r0, #0x46          /* 'F' */
    bl uart_send_byte
    ldr r3, [r7, #4]        /* Faulted PC saved by handler */
    bl send_word
    b flash_test_after

flash_ok:
    /* Success path */
    movs r0, #0x4F          /* 'O' */
    bl uart_send_byte
    bl send_word

flash_test_after:
    /* Dispatch based on phase */
    cmp r9, #0
    beq phase_0
    cmp r9, #1
    beq phase_1
    cmp r9, #2
    beq phase_2
    cmp r9, #3
    beq phase_3
    b phase_done       /* unknown phase, bail */


/* ==================== PHASE 0: Rapid STOP/wake cycling ==================== */
phase_0:
    /* Send 'C' = starting STOP cycling */
    movs r0, #0x43
    bl uart_send_byte
    /* Wait for TC (transmit complete) before STOP mode kills clocks */
    ldr r0, =0x40013800     /* USART1_SR */
p0_tc_wait:
    ldr r1, [r0]
    tst r1, #0x40            /* TC bit */
    beq p0_tc_wait

    /* === RTC setup for STOP mode wakeup === */

    /* 1. Enable LSI oscillator */
    ldr r0, =0x40021024    /* RCC_CSR */
    ldr r1, [r0]
    orr r1, r1, #1          /* LSION */
    str r1, [r0]
p0_lsi_wait:
    ldr r1, [r0]
    tst r1, #2              /* LSIRDY */
    beq p0_lsi_wait

    /* 2. Backup domain reset for clean RTC state */
    ldr r0, =0x40021020    /* RCC_BDCR */
    ldr r1, [r0]
    orr r1, r1, #0x10000    /* BDRST */
    str r1, [r0]
    bic r1, r1, #0x10000
    str r1, [r0]

    /* 3. Select LSI and enable RTC */
    ldr r0, =0x40021020    /* RCC_BDCR */
    movw r1, #0x8200        /* RTCEN + RTCSEL=LSI */
    str r1, [r0]

    /* 4. Wait for RSF */
    ldr r0, =0x40002804    /* RTC_CRL */
    ldr r1, [r0]
    bic r1, r1, #8
    str r1, [r0]
p0_rsf_wait:
    ldr r1, [r0]
    tst r1, #8
    beq p0_rsf_wait

    /* 5. Configure RTC: PRL=0, CNT=0, ALR=0 */
    ldr r0, =0x40002804    /* RTC_CRL */
p0_rtoff_pre:
    ldr r1, [r0]
    tst r1, #0x20
    beq p0_rtoff_pre
    orr r1, r1, #0x10       /* CNF */
    str r1, [r0]
    ldr r0, =0x40002808    /* RTC_PRLH */
    movs r1, #0
    str r1, [r0]
    ldr r0, =0x4000280C    /* RTC_PRLL */
    movs r1, #0
    str r1, [r0]
    ldr r0, =0x40002818    /* RTC_CNTH */
    movs r1, #0
    str r1, [r0]
    ldr r0, =0x4000281C    /* RTC_CNTL */
    movs r1, #0
    str r1, [r0]
    ldr r0, =0x40002820    /* RTC_ALRH */
    movs r1, #0
    str r1, [r0]
    ldr r0, =0x40002824    /* RTC_ALRL */
    movs r1, #0
    str r1, [r0]
    ldr r0, =0x40002804
    ldr r1, [r0]
    bic r1, r1, #0x10
    str r1, [r0]
p0_rtoff_pre2:
    ldr r1, [r0]
    tst r1, #0x20
    beq p0_rtoff_pre2

    /* 6. Enable RTC alarm interrupt */
    ldr r0, =0x40002800    /* RTC_CRH */
    movs r1, #2             /* ALRIE */
    str r1, [r0]

    /* 7. EXTI line 17 + NVIC IRQ 41 */
    ldr r0, =0x40010400    /* EXTI_IMR */
    ldr r1, [r0]
    orr r1, r1, #0x20000
    str r1, [r0]
    ldr r0, =0x40010408    /* EXTI_RTSR */
    ldr r1, [r0]
    orr r1, r1, #0x20000
    str r1, [r0]
    ldr r0, =0xE000E104    /* NVIC_ISER1 */
    movs r1, #(1 << 9)
    str r1, [r0]

    /* Rapid STOP cycling: 500 iterations */
    movs r10, #0
    ldr r11, =500

p0_stop_loop:
    cmp r10, r11
    bge p0_stop_done

    /* RSF sync before reading counter */
    ldr r0, =0x40002804
    ldr r1, [r0]
    bic r1, r1, #8
    str r1, [r0]
p0_rsf_loop:
    ldr r1, [r0]
    tst r1, #8
    beq p0_rsf_loop

    /* Set alarm = cnt + 4 */
    ldr r0, =0x40002804
p0_rtoff1:
    ldr r1, [r0]
    tst r1, #0x20
    beq p0_rtoff1
    orr r1, r1, #0x10       /* CNF */
    str r1, [r0]
    ldr r0, =0x4000281C    /* RTC_CNTL */
    ldr r2, [r0]
    adds r2, r2, #4
    ldr r0, =0x40002824    /* RTC_ALRL */
    str r2, [r0]
    ldr r0, =0x40002804
    ldr r1, [r0]
    bic r1, r1, #0x10
    str r1, [r0]
p0_rtoff2:
    ldr r1, [r0]
    tst r1, #0x20
    beq p0_rtoff2

    /* Clear ALRF + EXTI + NVIC pending */
    ldr r1, [r0]
    bic r1, r1, #2
    str r1, [r0]
    ldr r0, =0x40010414    /* EXTI_PR */
    ldr r1, =0x20000
    str r1, [r0]
    ldr r0, =0xE000E284    /* NVIC_ICPR1 */
    movs r1, #(1 << 9)
    str r1, [r0]

    /* STOP mode: LPDS=1, PDDS=0, SLEEPDEEP=1 */
    ldr r0, =0x40007000
    ldr r1, [r0]
    orr r1, r1, #1
    bic r1, r1, #2
    str r1, [r0]
    ldr r0, =0xE000ED10
    ldr r1, [r0]
    orr r1, r1, #4
    str r1, [r0]

    dsb
    wfi                      /* Enter STOP */

    /* Woken by RTC alarm */
    /* Clear SLEEPDEEP */
    ldr r0, =0xE000ED10
    ldr r1, [r0]
    bic r1, r1, #4
    str r1, [r0]

    add r10, r10, #1
    b p0_stop_loop

p0_stop_done:
    /* Send 'T' = cycling done, now testing */
    movs r0, #0x54
    bl uart_send_byte

    /* Flash read test after STOP cycling */
    ldr r7, =test_state
    movs r0, #1
    str r0, [r7]
    ldr r5, =0x08000000
    ldr r3, [r5]            /* May fault */
    movs r0, #0
    str r0, [r7]

    ldr r0, [r7]
    cmp r0, #0
    beq p0_flash_ok
    movs r0, #0x46          /* 'F' */
    bl uart_send_byte
    ldr r3, [r7, #4]
    bl send_word
    b p0_next

p0_flash_ok:
    movs r0, #0x4F          /* 'O' */
    bl uart_send_byte
    bl send_word

p0_next:
    /* Set phase = 1, trigger IWDG */
    movs r0, #1
    str r0, [r8]

    /* Send 'N' = next (about to reset) */
    movs r0, #0x4E
    bl uart_send_byte
    /* Wait for TX complete (USART1_SR at base+0) */
    ldr r0, =0x40013800
p0_txwait:
    ldr r1, [r0]
    tst r1, #0x40
    beq p0_txwait

    /* Enable IWDG with shortest timeout */
    ldr r0, =0x40003000    /* IWDG_KR */
    ldr r1, =0x5555        /* Enable register access */
    str r1, [r0]
    ldr r0, =0x40003004    /* IWDG_PR */
    movs r1, #0             /* prescaler /4 = fastest */
    str r1, [r0]
    ldr r0, =0x40003008    /* IWDG_RLR */
    movs r1, #1             /* reload = 1 = ~0.1ms timeout */
    str r1, [r0]
    ldr r0, =0x40003000    /* IWDG_KR */
    ldr r1, =0xCCCC        /* Start IWDG */
    str r1, [r0]

    /* Wait for IWDG to fire */
p0_wait_iwdg:
    b p0_wait_iwdg


/* ==================== PHASE 1: Post-IWDG reset ==================== */
phase_1:
    /* Already tested flash above. Now trigger WWDG */
    movs r0, #2
    str r0, [r8]

    /* Send 'N' = next */
    movs r0, #0x4E
    bl uart_send_byte
    ldr r0, =0x40013800
p1_txwait:
    ldr r1, [r0]
    tst r1, #0x40
    beq p1_txwait

    /* Enable WWDG clock */
    ldr r0, =0x4002101C    /* RCC_APB1ENR */
    ldr r1, [r0]
    orr r1, r1, #0x800     /* WWDGEN */
    str r1, [r0]

    /* WWDG_CFR: WDGTB=3 (div8), window=0x40 (tight) */
    ldr r0, =0x40002C04    /* WWDG_CFR */
    ldr r1, =0x0180        /* WDGTB=3, W=0x00 — very tight window */
    str r1, [r0]

    /* WWDG_CR: WDGA=1 (activate), T=0x7F (max counter) */
    ldr r0, =0x40002C00    /* WWDG_CR */
    movs r1, #0xFF          /* WDGA | T=0x7F */
    str r1, [r0]

    /* Don't refresh — let it count down and reset */
p1_wait_wwdg:
    b p1_wait_wwdg


/* ==================== PHASE 2: Post-WWDG reset ==================== */
phase_2:
    /* Already tested flash above. Now try STANDBY */
    movs r0, #3
    str r0, [r8]

    /* Send 'N' = next */
    movs r0, #0x4E
    bl uart_send_byte
    ldr r0, =0x40013800
p2_txwait:
    ldr r1, [r0]
    tst r1, #0x40
    beq p2_txwait

    /* Enable LSI for RTC wakeup */
    ldr r0, =0x40021024    /* RCC_CSR */
    ldr r1, [r0]
    orr r1, r1, #1
    str r1, [r0]
p2_lsi:
    ldr r1, [r0]
    tst r1, #2
    beq p2_lsi

    /* Enable RTC */
    ldr r0, =0x40021020    /* RCC_BDCR */
    ldr r1, [r0]
    tst r1, #0x8000
    bne p2_rtc_ok
    bic r1, r1, #0x300
    orr r1, r1, #0x200
    str r1, [r0]
    orr r1, r1, #0x8000
    str r1, [r0]
p2_rtc_ok:

    /* Set alarm = cnt + 40 (~1ms) for wakeup */
    ldr r0, =0x40002804    /* RTC_CRL */
p2_rtoff1:
    ldr r1, [r0]
    tst r1, #0x20
    beq p2_rtoff1
    orr r1, r1, #0x10
    str r1, [r0]

    ldr r0, =0x4000281C    /* RTC_CNTL */
    ldr r2, [r0]
    adds r2, r2, #40
    ldr r0, =0x40002824    /* RTC_ALRL */
    str r2, [r0]

    ldr r0, =0x40002804
    ldr r1, [r0]
    bic r1, r1, #0x10
    str r1, [r0]
p2_rtoff2:
    ldr r1, [r0]
    tst r1, #0x20
    beq p2_rtoff2

    /* Enable RTC alarm wakeup */
    ldr r0, =0x40002800
    ldr r1, [r0]
    orr r1, r1, #2
    str r1, [r0]

    /* Clear alarm and wakeup flags */
    ldr r0, =0x40002804
    ldr r1, [r0]
    bic r1, r1, #2
    str r1, [r0]
    ldr r0, =0x40007000    /* PWR_CR */
    ldr r1, [r0]
    bic r1, r1, #1          /* Clear CWUF */
    str r1, [r0]

    /* Enter STANDBY: PDDS=1, SLEEPDEEP=1, WFI */
    ldr r0, =0x40007000    /* PWR_CR */
    ldr r1, [r0]
    orr r1, r1, #2          /* PDDS = 1 (STANDBY) */
    bic r1, r1, #1          /* LPDS don't care */
    str r1, [r0]

    /* Clear WUF */
    orr r1, r1, #4          /* CWUF */
    str r1, [r0]

    ldr r0, =0xE000ED10    /* SCB_SCR */
    ldr r1, [r0]
    orr r1, r1, #4          /* SLEEPDEEP */
    str r1, [r0]

    dsb
    wfi                      /* Enter STANDBY — 1.8V domain off */

    /* Should not reach here */
p2_standby_wait:
    b p2_standby_wait


/* ==================== PHASE 3: Post-STANDBY (if we get here) ==================== */
phase_3:
    /* Already tested flash above */
    /* Fall through to done */

/* ==================== DONE ==================== */
phase_done:
    /* Clear BKP_DR1 for next run */
    movs r0, #0
    str r0, [r8]

    /* Send "DONE" */
    movs r0, #0x44     /* 'D' */
    bl uart_send_byte
    movs r0, #0x4F     /* 'O' */
    bl uart_send_byte
    movs r0, #0x4E     /* 'N' */
    bl uart_send_byte
    movs r0, #0x45     /* 'E' */
    bl uart_send_byte

done_loop:
    b done_loop


/* ========== HardFault handler ========== */
.thumb_func
hardfault_handler:
    /* Minimal handler: skip fault + flag-clearing code, save PC.
     * Skips 6 bytes: ldr(2) + movs(2) + str(2) so fault flag stays set. */
    tst lr, #4
    ite eq
    mrseq r0, msp
    mrsne r0, psp
    /* Save faulted PC to test_state[4] */
    ldr r1, [r0, #24]       /* Stacked PC */
    ldr r2, =test_state
    str r1, [r2, #4]
    /* Skip 6 bytes: faulting ldr(2) + movs r0,#0(2) + str r0,[r7](2) */
    adds r1, r1, #6
    str r1, [r0, #24]
    bx lr                    /* Exception return */

/* ========== RTC Alarm IRQ handler (for STOP wakeup — just return) ========== */
.thumb_func
rtc_alarm_isr:
    /* Clear alarm flag */
    ldr r0, =0x40002804
    ldr r1, [r0]
    bic r1, r1, #2
    str r1, [r0]
    /* Clear EXTI pending */
    ldr r0, =0x40010414
    ldr r1, =0x20000
    str r1, [r0]
    bx lr


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


/* ========== Data sections in SRAM ========== */
.align 8                    /* 256-byte alignment required by VTOR */
hardfault_vector_table:
    .space 232              /* 58 vectors (16 core + 42 IRQ for RTC alarm at IRQ41) */

test_state:
    .space 8
