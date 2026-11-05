# MirixOS build system
# Kernel: clang (freestanding) + ld.lld, custom linker script.
# ISO:    tools/mkiso.py (xorriso, BIOS El Torito + UEFI).
# Run:    QEMU with OVMF (UEFI) or SeaBIOS (BIOS).

BUILD      := build
KERNEL_ELF := $(BUILD)/kernel.elf

CC         := clang
CFLAGS     := --target=x86_64-unknown-none \
              -ffreestanding -fno-stack-protector -fno-stack-check \
              -fno-pic -fno-pie -mno-red-zone -mcmodel=kernel \
              -mno-mmx -mno-sse -mno-sse2 \
              -O2 -g -Wall -Wextra -std=c17 \
              -I. -Ikernel
LDFLAGS    := -T kernel/linker.ld

KERNEL_SRCS := $(shell find kernel -name '*.c')
KERNEL_OBJS := $(patsubst kernel/%.c,$(BUILD)/obj/%.o,$(KERNEL_SRCS))

QEMU       := qemu-system-x86_64
OVMF_CODE  := /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS  := /usr/share/OVMF/OVMF_VARS_4M.fd
QEMU_UEFI  := -machine q35 -cpu qemu64 -m 512M -cdrom $(BUILD)/mirixos.iso \
              -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
              -drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd \
              -no-reboot
QEMU_BIOS  := -machine q35 -cpu qemu64 -m 512M -cdrom $(BUILD)/mirixos.iso -no-reboot

.PHONY: all iso run run-bios run-headless debug test-boot clean

all: $(KERNEL_ELF)

$(BUILD)/obj/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL_ELF): $(KERNEL_OBJS) kernel/linker.ld
	@mkdir -p $(BUILD)
	ld.lld $(LDFLAGS) -o $@ $(KERNEL_OBJS)

iso: $(KERNEL_ELF)
	python3 tools/mkiso.py

# Interactive UEFI boot with GUI (WSLg) and serial on the terminal.
run: iso
	cp -f $(OVMF_VARS) $(BUILD)/OVMF_VARS.fd
	$(QEMU) $(QEMU_UEFI) -serial stdio

# Same, but BIOS boot (SeaBIOS) instead of UEFI.
run-bios: iso
	$(QEMU) $(QEMU_BIOS) -serial stdio

# Headless boot: no window, serial to build/serial.log (for scripts/CI).
run-headless: iso
	cp -f $(OVMF_VARS) $(BUILD)/OVMF_VARS.fd
	$(QEMU) $(QEMU_UEFI) -display none -serial file:$(BUILD)/serial.log

# Automated boot test: boots headless, greps serial log for the banner.
test-boot: iso
	bash tools/test_boot.sh

# GDB: QEMU waits for a debugger on :1234 (gdb -ex 'target remote :1234').
debug: iso
	cp -f $(OVMF_VARS) $(BUILD)/OVMF_VARS.fd
	$(QEMU) $(QEMU_UEFI) -serial stdio -s -S

clean:
	rm -rf $(BUILD)
