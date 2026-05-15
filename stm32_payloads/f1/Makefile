# STM32F103 payloads
CROSS_COMPILE ?= arm-none-eabi-
AS = $(CROSS_COMPILE)gcc
OBJCOPY = $(CROSS_COMPILE)objcopy
OBJDUMP = $(CROSS_COMPILE)objdump
SIZE = $(CROSS_COMPILE)size

TARGETS = led_payload bootrom_trace

all: $(addsuffix .bin,$(TARGETS)) payload_hex.h bootrom_trace_hex.h

# LED payload — SRAM boot (0x20000000)
led_payload.elf: led_payload.S
	$(AS) -mcpu=cortex-m3 -mthumb -nostartfiles -Wl,--section-start=.text=0x20000000 -o $@ $<
	$(SIZE) $@

# Boot ROM trace — SRAM (0x20004000, leaves lower SRAM free)
bootrom_trace.elf: bootrom_trace.S
	$(AS) -mcpu=cortex-m3 -mthumb -nostartfiles -Wl,--section-start=.text=0x20004000 -o $@ $<
	$(SIZE) $@

%.bin: %.elf
	$(OBJCOPY) -O binary $< $@
	@echo "Built $@ ($$(stat -c%s $@) bytes)"

payload_hex.h: led_payload.bin
	@echo "/* Auto-generated from led_payload.S */" > $@
	@echo "static const uint8_t f103_led_payload[] = {" >> $@
	@xxd -i < $< >> $@
	@echo "};" >> $@
	@echo "Generated $@"

bootrom_trace_hex.h: bootrom_trace.bin
	@echo "/* Auto-generated from bootrom_trace.S */" > $@
	@echo "static const uint8_t f103_bootrom_trace[] = {" >> $@
	@xxd -i < $< >> $@
	@echo "};" >> $@
	@echo "Generated $@"

disasm-%: %.elf
	$(OBJDUMP) -d -t $<

clean:
	rm -f $(addsuffix .elf,$(TARGETS)) $(addsuffix .bin,$(TARGETS)) payload_hex.h bootrom_trace_hex.h

.PHONY: all clean
