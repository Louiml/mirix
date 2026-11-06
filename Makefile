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
              -I. -Ikernel -I$(BUILD)
LDFLAGS    := -T kernel/linker.ld

# User programs (ring 3): freestanding, linked low at 0x400000, embedded into
# the kernel as C arrays (build/<name>_elf.h) and loaded by kernel/proc/elf.c.
USER_CFLAGS := --target=x86_64-unknown-none \
              -ffreestanding -fno-stack-protector -fno-stack-check \
              -fno-pic -fno-pie -mno-red-zone \
              -mno-mmx -mno-sse -mno-sse2 \
              -O2 -g -Wall -Wextra -std=c17 \
              -I. -Iuser
USER_PROGS  := hello shell aurora
USER_HDRS   := $(patsubst %,$(BUILD)/%_elf.h,$(USER_PROGS))

# Embedded TrueType font (Phase 6c): assets/fonts/DejaVuSans.ttf -> C array.
FONT_HDR    := $(BUILD)/DejaVuSans_ttf.h
$(FONT_HDR): assets/fonts/DejaVuSans.ttf tools/elf2h.py
	@mkdir -p $(BUILD)
	python3 tools/elf2h.py $< dejavu_sans_ttf > $@

KERNEL_SRCS := $(shell find kernel -name '*.c')
KERNEL_ASMS := $(shell find kernel -name '*.asm')
KERNEL_OBJS := $(patsubst kernel/%.c,$(BUILD)/obj/%.o,$(KERNEL_SRCS)) \
               $(patsubst kernel/%.asm,$(BUILD)/obj-asm/%.o,$(KERNEL_ASMS))
# generated headers must exist before compiling kernel objects that include them
$(KERNEL_OBJS): | $(USER_HDRS) $(FONT_HDR)

QEMU       := qemu-system-x86_64
OVMF_CODE  := /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS  := /usr/share/OVMF/OVMF_VARS_4M.fd
QEMU_UEFI  := -machine q35 -cpu qemu64 -m 512M -cdrom $(BUILD)/mirixos.iso \
              -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
              -drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd \
              -no-reboot
QEMU_BIOS  := -machine q35 -cpu qemu64 -m 512M -cdrom $(BUILD)/mirixos.iso -no-reboot

.PHONY: all iso run run-bios run-headless debug test-boot test-crash clean

all: $(KERNEL_ELF)

$(BUILD)/obj/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj-asm/%.o: kernel/%.asm
	@mkdir -p $(dir $@)
	nasm -f elf64 -g -F dwarf $< -o $@

$(KERNEL_ELF): $(KERNEL_OBJS) $(USER_HDRS) $(FONT_HDR) kernel/linker.ld
	@mkdir -p $(BUILD)
	ld.lld $(LDFLAGS) -o $@ $(KERNEL_OBJS)

# user programs: user/<name>.c + shared start.asm, linked with user/<name>.ld,
# then embedded as build/<name>_elf.h
$(BUILD)/obj-user/start.o: user/start.asm
	@mkdir -p $(dir $@)
	nasm -f elf64 -g -F dwarf $< -o $@

$(BUILD)/obj-user/%.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/%.elf: $(BUILD)/obj-user/start.o $(BUILD)/obj-user/%.o user/%.ld
	@mkdir -p $(BUILD)
	ld.lld -T user/$*.ld -nostdlib -static -o $@ $(BUILD)/obj-user/start.o $(BUILD)/obj-user/$*.o

$(BUILD)/%_elf.h: $(BUILD)/%.elf
	python3 tools/elf2h.py $< $*_elf > $@

iso: $(KERNEL_ELF)
	python3 tools/mkiso.py

# Raw disk image attached to QEMU's AHCI (ide-hd on ide.0) for the 4c test.
$(BUILD)/mirix.img: tools/mkdisk.py
	@mkdir -p $(BUILD)
	python3 tools/mkdisk.py $@

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
test-boot: iso $(BUILD)/mirix.img
	bash tools/test_boot.sh

# Panic-path test: boots a crash-cmdline ISO, expects a #PF panic screen.
test-crash: $(BUILD)/mirix.img
	MIRIXOS_CMDLINE=crash MIRIXOS_ISO_NAME=mirixos-crash.iso python3 tools/mkiso.py
	python3 tools/test_crash.py

# GDB: QEMU waits for a debugger on :1234 (gdb -ex 'target remote :1234').
debug: iso
	cp -f $(OVMF_VARS) $(BUILD)/OVMF_VARS.fd
	$(QEMU) $(QEMU_UEFI) -serial stdio -s -S

clean:
	rm -rf $(BUILD)
