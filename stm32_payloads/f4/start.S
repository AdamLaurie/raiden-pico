
/*
 * Entrypoint for both stages.
 *
 * Stage 1 is executed from SRAM after booting into SRAM mode (BOOT1: 1, BOOT0: 1).
 * The FPB is activated to ensure the next boot enters stage 2, then enters an infinite
 * loop waiting for power to be cut. Once the system reboots, it enters normal flash boot
 * (BOOT1: 0, BOOT0: 0), the FPB patches address 0x00000004 to redirect to SRAM, and
 * the system enters stage 2.
 */
.global _start_stage1
.global _start_stage2
.global _start
.syntax divided
.thumb

//#define SKIP_STAGE1

.section .isr_vector
.thumb
isr_vectors:
	/* The STM32F4 will consult this address to determine the stack pointer address. */
	/* Must be above bootloader reserved area (0x20000000-0x20003FFF) and within our RAM */
	.word 0x20009000 // 0x00 - End of our 20K allocation at 0x20004000
	/* The STM32F4 will consult this address to determine the entrypoint */
#ifdef SKIP_STAGE1
	.word _start_stage2+1 // 0x04
#else
	.word _start_stage1+1 // 0x04
#endif
	/* The next six words serve double-duty: They act as a normal vector table,
	 * and they pad it out so that the word pointing to _start_stage2 is at offset
	 * 0x20 from the start.
	 */
	.word Handle_NMI+1 // 0x08
	.word Handle_HardFault+1 // 0x0c
	.word Handle_MemoryManage+1 // 0x10
	.word 0 // 0x14
	.word 0 // 0x18
	.word 0 // 0x1c
/* These is the stage 2 entry point. It must be at offset 0x20, to ensure
 * it ends up at 0x20000020 so the FPB can refer to it. At startup, the CPU
 * will load this value into the program counter instead of the address at
 * 0x00000004. Ensure the LSB is set as it is Thumb2 code.
 */
stage2_pointer:
	.word _start_stage2+1 // 0x20

/* For STM32F1, a hardcoded entrypoint is used. This entrypoint may be any one of the following:
 *	- 0x20000108
 *	- 0x200001cc
 *	- 0x200001e0
 * depending on how many interrupt vectors a particular target has. To work around this,
 * we fill the entire address with a nop sled to ensure the entrypoint is positioned at least
 * at 0x200001e0.
 */

/* nop sled to ensure _start_stage1 is located after 0x200001e0 */
nop_sled:
.rept 256
	nop
.endr

#ifndef SKIP_STAGE1
_start_stage1: /* stage 1 */
	/* FPB Address Space */
	ldr r0, =0xe0002000

	/* FP_CTRL: Enable FPB (bit 0) with key set (bit 1) */
	movs r1, #3

	/* FP_REMAP: All addresses are relative to 0x4020 in the system space (i.e. 0x20004020) */
	/* Payload is at 0x20004000, stage2_pointer is at offset 0x20 */
	ldr r2, =0x4020

	/* FP_COMP0: Enable (bit 0) and remap address 4
	 * Since this is comparison 0, it will remap to the remap address + 0,
	 * i.e. 0x20004020
	 */
	movs r3, #0x05

	/* Write all three values into the FPB address space */
	stm r0!, {r1, r2, r3}

	/* Set stack pointer by reading it out of the vector table */
	ldr r0, =isr_vectors
	ldr r0, [r0]
	mov sp, r0

	/* Clear BSS */
	movs r0, #0
	ldr  r1, =_bss_start
	ldr  r2, =_bss_end
bss_loop1:
	str  r0, [r1]
	add r1, #4
	cmp  r1, r2
	blt bss_loop1

	/* NOTE: There is no .data section as we're running from RAM -- it's all .text */

	bl main_stage1
	b .
#endif

_start:
_start_stage2:
	/* Set stack pointer by reading it out of the vector table */
	ldr r0, =isr_vectors
	ldr r0, [r0]
	mov sp, r0
	
#if 0
	/* Disable the FPB to make debugging easier */
	ldr r0, =0xe0002000
	movs r1, #2
	str r1, [r0]
	
	/* Configure VTOR */
	ldr r0, =0xE000ED08
	ldr r1, =isr_vectors
	str r1, [r0]

	/* Enable interrupts */
	cpsie if
#endif

	/* Clear BSS */
	movs r0, #0
	ldr  r1, =_bss_start
	ldr  r2, =_bss_end
bss_loop2:
	str  r0, [r1]
	add r1, #4
	cmp  r1, r2
	blt bss_loop2

	/* NOTE: There is no .data section as we're running from RAM -- it's all .text */

	bl main_stage2
	b .

Handle_NMI:
Handle_HardFault:
Handle_MemoryManage:
	mrs r0, IPSR
	b alert_crash
