#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define __IO volatile
static const char DUMP_START_MAGIC[] = {0x10, 0xAD, 0xDA, 0x7A};

static volatile uint32_t *IDCODE = (uint32_t *)0xe0042000;

static uint8_t iwdg_enabled;
#define _IWDG_KR (*(uint16_t *)0x40003000)
#if 0

#define _WDG_SW                \
	(*(uint32_t *)0x1FFFF800 & \
		1UL << 16) // Page 20: https://www.st.com/resource/en/programming_manual/pm0075-stm32f10xxx-flash-memory-microcontrollers-stmicroelectronics.pdf

#else

#define _WDG_SW                \
	(*(uint32_t *)0x1FFFC000 & \
		1UL << 5) // Page 20: https://www.st.com/resource/en/programming_manual/pm0075-stm32f10xxx-flash-memory-microcontrollers-stmicroelectronics.pdf

#endif

// GPIO
typedef struct __attribute__((packed)) {
	__IO uint32_t MODER;
	__IO uint32_t OTYPER;
	__IO uint32_t OSPEEDR;
	__IO uint32_t PUPDR;
	__IO uint32_t IDR;
	__IO uint32_t ODR;
	__IO uint32_t BSRR;
	__IO uint32_t LCKR;
	__IO uint32_t AFRL;
	__IO uint32_t AFRH;
} GPIO;

// USART
typedef struct __attribute__((packed)) {
	__IO uint32_t SR;
	__IO uint32_t DR;
	__IO uint32_t BRR;
	__IO uint32_t CR1;
	__IO uint32_t CR2;
	__IO uint32_t CR3;
	__IO uint32_t GTPR;
} USART;

#define RCC_AHB1ENR        (*(volatile uint32_t *)0x40023830u)
#define RCC_AHB1ENR_GPIOC  (1 << 2)
#define RCC_APB1ENR        (*(volatile uint32_t *)0x40023840u)
#define RCC_APB1ENR_USART3 (1 << 18)

#define SYSCFG_MEMRMP (*(volatile uint32_t *)0x40013800u)
#define FLASH_OPTCR   (*(volatile uint32_t *)0x40023c14u)
#define SCB_AIRCR   (*(volatile uint32_t *)0xE000ED0Cu)

#define GPIOA ((GPIO *)0x40020000u)
#define GPIOB ((GPIO *)0x40020400u)
#define GPIOC ((GPIO *)0x40020800u)

#define USART3 ((USART *)0x40004800u)

#define PIN_CONFIG_ALT_FUNCTION  2
#define PIN_CONFIG_INPUT_PULL_UP 0x8

// #define USARTDIV      0x00000341u // 9600 baud @ 8Mhz
#define USARTDIV      0x00000682u // 9600 baud @ 16Mhz
// #define USARTDIV      0x0000008Bu // 115200 baud @ 16Mhz (HSI default)
// #define USARTDIV      0x00000045u // 115200 baud @ 8Mhz
// #define USART_CR1_MSK 0x0000200Cu // 8N1: 8-bit, no parity, enable TX+UE
#define USART_CR1_MSK 0x0000340Cu // 8E1: 9-bit word (8+parity), even parity, enable TX+UE
#define USART_SR_TXE  (1 << 7)

#define FP_CTRL  (*(volatile uint32_t *)0xe0002000u)
#define FP_REMAP (*(volatile uint32_t *)0xe0002004u)
#define FP_COMP0 (*(volatile uint32_t *)0xe0002008u)
#define FP_COMP1 (*(volatile uint32_t *)0xe000200cu)

static void delay(uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		__asm__("nop");
	}
}

static void init_uart(bool stage_2)
{
	(void)stage_2;
	/* Enable Clocks */
	RCC_AHB1ENR |= RCC_AHB1ENR_GPIOC;
	RCC_APB1ENR |= RCC_APB1ENR_USART3;

	/* Configure Pins */

	// Set PC10 (TX) to alternate function 7 (USART3) push-pull
	GPIOC->MODER &= ~(0x3 << 20);  // Clear bits 21:20 for PC10
	GPIOC->MODER |= (PIN_CONFIG_ALT_FUNCTION << 20);
	GPIOC->AFRH &= ~(0xF << 8);    // Clear AF bits for PC10 (AFRH bit 8-11)
	GPIOC->AFRH |= (7 << 8);       // AF7 for USART3

	/* Configure and enable USART3 */
	USART3->BRR = USARTDIV;
	USART3->CR1 = USART_CR1_MSK;
}

void refresh_iwdg(void)
{
	if (iwdg_enabled) {
		_IWDG_KR = 0xAAAA;
	}
}

static const uint8_t txtMap[] = "0123456789ABCDEF";

// Writes character to USART
static void writeChar(uint8_t const chr)
{
	while (!(USART3->SR & USART_SR_TXE)) {
		refresh_iwdg(); // A byte takes ~1ms to be send at 9600, so there's plenty of time to reset the IWDG
						/* wait */
	}

	USART3->DR = chr;
}

// Writes byte to USART
static void writeByte(uint8_t b)
{
	writeChar(txtMap[b >> 4]);
	writeChar(txtMap[b & 0x0F]);
}

// Writes word to USART
static void writeWord(uint32_t const word)
{
	writeChar((word & 0x000000FF));
	writeChar((word & 0x0000FF00) >> 8);
	writeChar((word & 0x00FF0000) >> 16);
	writeChar((word & 0xFF000000) >> 24);
}

static void writeWordLe(uint32_t const word)
{
	writeByte((word & 0xFF000000) >> 24);
	writeByte((word & 0x00FF0000) >> 16);
	writeByte((word & 0x0000FF00) >> 8);
	writeByte((word & 0x000000FF));
}

// Writes string to USART
static void writeStr(char const *const str)
{
	uint32_t ind = 0u;

	while (str[ind]) {
		writeChar(str[ind]);
		++ind;
	}
}

int main_stage1(void)
{
	// Stage 1: FPB is already set up by start.S
	// Do NOT access any peripherals here - RDP blocks it when running via Go command
	// Just wait for reset. After reset with BOOT0=LOW, FPB will redirect to stage 2.
	while (1) {
		// Wait for power cycle / reset
	}
}

int main_stage2(void)
{
	init_uart(true);
	writeStr("Hello from stage 2!");
	iwdg_enabled = (_WDG_SW == 0); // Check WDG_SW bit.
	refresh_iwdg();
	writeStr("\r\nSYSCFG_MEMRMP: 0x");
	writeWordLe(SYSCFG_MEMRMP);
	writeStr("\r\n");

	// while (1) {
	// 	writeChar('U');
	// 	delay(500000);
	// }
	/* Print start magic to inform the attack board that
	   we are going to dump */
	for (uint32_t i = 0; i < sizeof(DUMP_START_MAGIC); i++) {
		writeChar(DUMP_START_MAGIC[i]);
	}

	uint32_t const *addr = (uint32_t *)0x08000000;
	while (((uintptr_t)addr) <
		(0x08000000U +
			(1024UL *
				1024UL))) // Try dumping up to 1M. When reaching unimplemented memory, it will cause hard fault and stop.
	{
		writeWord(*addr);
		++addr;
	}

	while (1) // End
	{
		refresh_iwdg(); // Keep refreshing IWDG to prevent reset
	}
}

void alert_crash(uint32_t crash_id)
{
	init_uart(true);
	(void)crash_id;
	writeStr("!!! EXCEPTION !!!\r\nID: ");
	writeByte(crash_id);
	writeStr("\r\nRestart required!\r\n\r\n");
	// SCB_AIRCR = 0x05FA0004u;
	// static int error_count = 0;
	// led1(true);
	// led2(false);
	while (1) {
		// led3(!!(error_count & 2));
		// led4(!!(error_count & 4));
		// error_count += 1;
		delay(1000);
	}
}
