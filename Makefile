# Makefile — build Vaporware firmware examples for N32G031K8Q7-1
#
# Requires: arm-none-eabi-gcc in PATH (sudo apt install gcc-arm-none-eabi on Ubuntu)
# Usage:
#   make flappy          → firmware/flappy.bin
#   make slots           → firmware/slots.bin
#   make all             → all of the above
#   make clean

CROSS    := arm-none-eabi-
CC       := $(CROSS)gcc
OBJCOPY  := $(CROSS)objcopy
SIZE     := $(CROSS)size

CFLAGS := \
    -mcpu=cortex-m0 \
    -mthumb \
    -Os \
    -Wall \
    -ffunction-sections \
    -fdata-sections \
    -fno-common \
    -Wno-misleading-indentation \
    -Wno-unused-function \
    -Ivaporware/src/include

LDFLAGS_BASE := \
    -mcpu=cortex-m0 \
    -mthumb \
    -Wl,--gc-sections \
    -T vaporware/src/n32g031.ld \
    -nostartfiles

# Vaporware library sources (shared by all examples)
VAPORWARE_SRC := \
    vaporware/src/src/startup.s \
    vaporware/src/src/system.c \
    vaporware/src/src/display.c \
    vaporware/src/src/button.c \
    vaporware/src/src/battery.c \
    vaporware/src/src/vape.c \
    vaporware/src/src/nv.c \
    vaporware/src/src/app.c

$(shell mkdir -p firmware)

# ── flappy ────────────────────────────────────────────────────────────────────
firmware/flappy.elf: $(VAPORWARE_SRC) vaporware/examples/flappy/src/flappy.c
	$(CC) $(CFLAGS) $(LDFLAGS_BASE) -Wl,-Map,$(@:.elf=.map) $^ -o $@
	$(SIZE) $@

firmware/flappy.bin: firmware/flappy.elf
	$(OBJCOPY) -O binary $< $@
	@echo "  → $@ ($$(wc -c < $@) bytes)"

.PHONY: flappy
flappy: firmware/flappy.bin

# ── slots ─────────────────────────────────────────────────────────────────────
firmware/slots.elf: $(VAPORWARE_SRC) vaporware/examples/slots/src/slots.c vaporware/examples/slots/src/main.c
	$(CC) $(CFLAGS) -Ivaporware/examples/slots/include $(LDFLAGS_BASE) -Wl,-Map,$(@:.elf=.map) $^ -o $@
	$(SIZE) $@

firmware/slots.bin: firmware/slots.elf
	$(OBJCOPY) -O binary $< $@
	@echo "  → $@ ($$(wc -c < $@) bytes)"

.PHONY: slots
slots: firmware/slots.bin

# ── template ──────────────────────────────────────────────────────────────────
firmware/template.elf: $(VAPORWARE_SRC) vaporware/examples/template/src/main.c
	$(CC) $(CFLAGS) $(LDFLAGS_BASE) -Wl,-Map,$(@:.elf=.map) $^ -o $@
	$(SIZE) $@

firmware/template.bin: firmware/template.elf
	$(OBJCOPY) -O binary $< $@
	@echo "  → $@ ($$(wc -c < $@) bytes)"

.PHONY: template
template: firmware/template.bin

# ── all / clean ───────────────────────────────────────────────────────────────
.PHONY: all
all: flappy slots template

.PHONY: clean
clean:
	rm -rf firmware/*.elf firmware/*.bin firmware/*.map
