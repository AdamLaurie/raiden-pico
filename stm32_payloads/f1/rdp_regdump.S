/*
 * STM32F103 RDP1 — Comprehensive Register Dump
 *
 * Dumps all registers relevant to flash protection state so we can
 * compare the glitched environment (flash reads work) vs the halt
 * environment (flash reads fault).
 *
 * Stage 1: STOP debug kill (clear C_DEBUGEN)
 * Stage 2: Dump registers via UART, then attempt flash reads with
 *          hardfault recovery to see exactly which reads succeed/fault.
 *
 * Protocol: "REG1" + N register words (4B each LE) + flash test results + "DONE"
 *
 * Register dump order (17 registers):
 *   0: DHCSR        (0xE000EDF0)  — C_DEBUGEN
 *   1: DEMCR        (0xE000EDFC)  — debug monitor
 *   2: DBGMCU_IDCODE (0xE0042000) — debug MCU ID
 *   3: DBGMCU_CR    (0xE0042004)  — debug MCU control
 *   4: FLASH_ACR    (0x40022000)  — flash access control
 *   5: FLASH_SR     (0x4002200C)  — flash status
 *   6: FLASH_CR     (0x40022010)  — flash control (LOCK bit)
 *   7: FLASH_OBR    (0x4002201C)  — option byte reg (RDPRT!)
 *   8: FLASH_WRPR   (0x40022020)  — write protection
 *   9: FP_CTRL      (0xE0002000)  — FPB config
 *  10: DWT_CTRL     (0xE0001000)  — DWT control
 *  11: SCB_AIRCR    (0xE000ED0C)  — app interrupt/reset
 *  12: SCB_SCR      (0xE000ED10)  — system control
 *  13: AFIO_MAPR    (0x40010004)  — AF remap
 *  14: RCC_CR       (0x40021000)  — clock control
 *  15: RCC_CSR      (0x40021024)  — reset flags
 *  16: CPUID        (0xE000ED00)  — CPU ID
 *
 * Then flash read tests (each: 1 byte status + 4 byte data if success):
 *   'O' + data(4B): read succeeded
 *   'F' + PC(4B):   read faulted (hardfault)
 *   Tests: 0x08000000, 0x08000004, 0x1FFFF800 (option bytes)
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

    /* Configure FPB — only COMP0 for reset vector redirect */
    ldr r0, =0xE0002000    /* FPB base */
    movs r1, #3             /* FP_CTRL: ENABLE + KEY */
    str r1, [r0, #0]
    movs r1, #0x20          /* FP_REMAP: base 0x20000020 */
    str r1, [r0, #4]
    /* COMP0: reset vector redirect (addr 0x04, REPLACE=00, ENABLE) */
    movs r1, #0x05
    str r1, [r0, #8]

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

    /* Wait for UART TX to finish */
    ldr r0, =0x40013800     /* USART1_SR */
9:  ldr r1, [r0]
    tst r1, #0x40            /* TC (transmit complete) */
    beq 9b

    /* Issue SYSRESETREQ via AIRCR — software reset, FPB stays configured */
    dsb
    ldr r0, =0xE000ED0C      /* SCB_AIRCR */
    ldr r1, =0x05FA0004      /* VECTKEY + SYSRESETREQ */
    str r1, [r0]
    dsb
stage1_wait:
    b stage1_wait             /* Should never reach here */


/* ========== STAGE 2: Comprehensive Register Dump ========== */
.align 2
stage2_start:
    ldr sp, =0x20005000

    /* Re-enable clocks */
    ldr r0, =0x40021018
    ldr r1, [r0]
    orrs r1, r1, #4
    orr r1, r1, #0x4000
    str r1, [r0]
    /* Also enable AFIO clock for AFIO_MAPR read */
    orr r1, r1, #1
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

    ldr r4, =0x40013800    /* USART1 base — keep in r4 */

    /* Set up hardfault handler in SRAM */
    ldr r6, =hardfault_vector_table
    ldr r0, =0x20005000
    str r0, [r6, #0]           /* SP */
    ldr r0, =hardfault_handler + 1
    str r0, [r6, #0x0C]        /* HardFault vector */
    ldr r0, =0xE000ED08
    str r6, [r0]                /* VTOR = SRAM table */

    /* --- Send header "REG1" --- */
    movs r0, #0x52     /* 'R' */
    bl uart_send_byte
    movs r0, #0x45     /* 'E' */
    bl uart_send_byte
    movs r0, #0x47     /* 'G' */
    bl uart_send_byte
    movs r0, #0x31     /* '1' */
    bl uart_send_byte

    /* --- Dump 17 registers --- */

    /* 0: DHCSR */
    ldr r5, =0xE000EDF0
    ldr r3, [r5]
    bl send_word

    /* 1: DEMCR */
    ldr r5, =0xE000EDFC
    ldr r3, [r5]
    bl send_word

    /* 2: DBGMCU_IDCODE */
    ldr r5, =0xE0042000
    ldr r3, [r5]
    bl send_word

    /* 3: DBGMCU_CR */
    ldr r5, =0xE0042004
    ldr r3, [r5]
    bl send_word

    /* 4: FLASH_ACR */
    ldr r5, =0x40022000
    ldr r3, [r5]
    bl send_word

    /* 5: FLASH_SR */
    ldr r5, =0x4002200C
    ldr r3, [r5]
    bl send_word

    /* 6: FLASH_CR */
    ldr r5, =0x40022010
    ldr r3, [r5]
    bl send_word

    /* 7: FLASH_OBR */
    ldr r5, =0x4002201C
    ldr r3, [r5]
    bl send_word

    /* 8: FLASH_WRPR */
    ldr r5, =0x40022020
    ldr r3, [r5]
    bl send_word

    /* 9: FP_CTRL */
    ldr r5, =0xE0002000
    ldr r3, [r5]
    bl send_word

    /* 10: DWT_CTRL */
    ldr r5, =0xE0001000
    ldr r3, [r5]
    bl send_word

    /* 11: SCB_AIRCR */
    ldr r5, =0xE000ED0C
    ldr r3, [r5]
    bl send_word

    /* 12: SCB_SCR */
    ldr r5, =0xE000ED10
    ldr r3, [r5]
    bl send_word

    /* 13: AFIO_MAPR */
    ldr r5, =0x40010004
    ldr r3, [r5]
    bl send_word

    /* 14: RCC_CR */
    ldr r5, =0x40021000
    ldr r3, [r5]
    bl send_word

    /* 15: RCC_CSR */
    ldr r5, =0x40021024
    ldr r3, [r5]
    bl send_word

    /* 16: CPUID */
    ldr r5, =0xE000ED00
    ldr r3, [r5]
    bl send_word

    /* --- Flash read tests with hardfault recovery --- */
    /* For each test:
     *   Set test_addr, test_result_ptr, then do the read.
     *   If it succeeds: send 'O' + data(4B)
     *   If hardfault: handler sends 'F' + stacked_PC(4B), then resumes at next test
     */

    /* Store return addresses for hardfault recovery */
    ldr r7, =test_state     /* r7 points to test state area in SRAM */

    /* Test 0: read 0x08000000 (flash base) */
    movs r0, #0
    str r0, [r7]            /* test_state.index = 0 */
    ldr r0, =flash_test_1
    str r0, [r7, #4]        /* test_state.resume = next test */
    ldr r5, =0x08000000
    ldr r3, [r5]            /* THE READ — may fault */
    /* If we get here, read succeeded */
    movs r0, #0x4F          /* 'O' */
    bl uart_send_byte
    bl send_word

flash_test_1:
    /* Test 1: read 0x08000004 (reset vector) */
    movs r0, #1
    str r0, [r7]
    ldr r0, =flash_test_2
    str r0, [r7, #4]
    ldr r5, =0x08000004
    ldr r3, [r5]
    movs r0, #0x4F          /* 'O' */
    bl uart_send_byte
    bl send_word

flash_test_2:
    /* Test 2: read 0x1FFFF800 (option bytes — RDP byte) */
    movs r0, #2
    str r0, [r7]
    ldr r0, =flash_test_3
    str r0, [r7, #4]
    ldr r5, =0x1FFFF800
    ldr r3, [r5]
    movs r0, #0x4F          /* 'O' */
    bl uart_send_byte
    bl send_word

flash_test_3:
    /* Test 3: read 0x1FFFF802 (option bytes — USER/nUSER) */
    movs r0, #3
    str r0, [r7]
    ldr r0, =flash_test_4
    str r0, [r7, #4]
    ldr r5, =0x1FFFF802
    ldr r3, [r5]
    movs r0, #0x4F          /* 'O' */
    bl uart_send_byte
    bl send_word

flash_test_4:
    /* Test 4: read 0x1FFFF7E0 (flash size register — should always work) */
    movs r0, #4
    str r0, [r7]
    ldr r0, =flash_tests_done
    str r0, [r7, #4]
    ldr r5, =0x1FFFF7E0
    ldr r3, [r5]
    movs r0, #0x4F          /* 'O' */
    bl uart_send_byte
    bl send_word

flash_tests_done:
    /* Send "DONE" */
    movs r0, #0x44     /* 'D' */
    bl uart_send_byte
    movs r0, #0x4F     /* 'O' */
    bl uart_send_byte
    movs r0, #0x4E     /* 'N' */
    bl uart_send_byte
    movs r0, #0x45     /* 'E' */
    bl uart_send_byte

3:  b 3b


/* HardFault handler: send 'F' + stacked PC, then resume at next test */
hardfault_handler:
    mov r12, lr
    ldr r4, =0x40013800
    tst lr, #4
    ite eq
    mrseq r5, msp
    mrsne r5, psp
    movs r0, #0x46          /* 'F' */
    bl uart_send_byte
    ldr r3, [r5, #24]       /* Stacked PC */
    bl send_word
    ldr r7, =test_state
    ldr r1, [r7, #4]
    orr r1, r1, #1
    str r1, [r5, #24]       /* Overwrite stacked PC */
    bx r12                   /* Proper EXC_RETURN */


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
.align 7                    /* 128-byte alignment for VTOR (16 entries) */
/* Hardfault vector table — placed at known SRAM address */
hardfault_vector_table:
    .space 64               /* 16 vectors * 4 bytes */

/* Test state: [0]=index, [4]=resume_addr */
test_state:
    .space 8
