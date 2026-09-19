# STM32F4 RDP Bypass Payload Makefile
# Based on xobs/stimpik

CROSS_COMPILE ?= arm-none-eabi-
CC = $(CROSS_COMPILE)gcc
OBJCOPY = $(CROSS_COMPILE)objcopy
SIZE = $(CROSS_COMPILE)size

CFLAGS = -mcpu=cortex-m4 -mthumb -Os -ffunction-sections -fdata-sections -Wall
LDFLAGS = -T payload.ld -nostartfiles -Wl,--gc-sections

TARGET = payload

all: $(TARGET).bin

$(TARGET).elf: start.S payload.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	$(SIZE) $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	@echo "Built $@ ($(shell stat -c%s $@) bytes)"

clean:
	rm -f $(TARGET).elf $(TARGET).bin

.PHONY: all clean

# ---------------------------------------------------------------------------
# RDP1 BYPASS payload driven by target_power_bypass() (per-family path).
# Loads at 0x20000000 (SRAM-boot alias). Plain protocol + FPB reader trick.
AS = $(CROSS_COMPILE)gcc

bypass: rdp_bypass_f4_hex.h

rdp_bypass.elf: rdp_bypass.S
	$(AS) -mcpu=cortex-m4 -mthumb -nostartfiles -Wl,--section-start=.text=0x20000000 -o $@ $<

rdp_bypass.bin: rdp_bypass.elf
	$(OBJCOPY) -O binary $< $@
	@echo "Built $@ ($(shell stat -c%s $@) bytes)"

rdp_bypass_f4_hex.h: rdp_bypass.bin
	@echo "/* Auto-generated from stm32_payloads/f4/rdp_bypass.S ($(shell stat -c%s $<) bytes) */" > $@
	@echo "/* STM32F4 RDP1 BYPASS payload. Loads at 0x20000000 (SRAM-boot alias base). */" >> $@
	@echo "/* Protocol: RDP1 + CPUID + flash. Uses FPB reader trick for F4 flash reads. */" >> $@
	@echo "static const uint8_t f4_rdp_bypass_payload[] = {" >> $@
	@xxd -i < $< >> $@
	@echo "};" >> $@
	@echo "Generated $@"

.PHONY: bypass
