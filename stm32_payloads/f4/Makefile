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
