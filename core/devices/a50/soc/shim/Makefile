# The Tensor G5's boot shim (README.md, "The boot shim"): shim.bin, an arm64 Linux Image that starts xnuloader without a UEFI.
# Needs LLVM (clang, ld.lld, llvm-objcopy for an ELF target: Apple's clang compiles, Homebrew's llvm links) and the common
# repository around it (socs/lga in hackios-xnu-kernel): the UEFI declarations are xnuloader/efi.h, the loader's own.
#
#   make            shim.bin
#   make test       the device-tree and memory-map host test (fdt-test), on tests/*.dts
#   make clean

ROOT     ?= $(abspath ../../../..)
LLVM     ?= $(firstword $(wildcard $(HOME)/llvm-mix /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin /usr/lib/llvm-19/bin /usr/lib/llvm-18/bin) /usr/bin)
CC        = $(LLVM)/clang
LD        = $(firstword $(wildcard $(LLVM)/ld.lld) $(shell command -v ld.lld))
OBJCOPY   = $(firstword $(wildcard $(LLVM)/llvm-objcopy) llvm-objcopy)
HOSTCC   ?= cc
DTC      ?= dtc

CFLAGS    = --target=aarch64-none-elf -ffreestanding -fno-builtin -nostdlib -fPIE -fvisibility=hidden \
            -mgeneral-regs-only -mstrict-align -O2 -Wall -Wextra -Wno-unused-parameter -Wno-pointer-to-int-cast \
            -I$(ROOT)/xnuloader -I.
OBJS      = start.o shim.o fdt.o memmap.o

all: shim.bin

%.o: %.c fdt.h memmap.h $(ROOT)/xnuloader/efi.h
	$(CC) $(CFLAGS) -c $< -o $@

start.o: start.S
	$(CC) $(CFLAGS) -c $< -o $@

shim.elf: $(OBJS) shim.ld
	$(LD) -pie --no-dynamic-linker -z norelro -z max-page-size=4096 --hash-style=sysv -T shim.ld $(OBJS) -o $@

# .bss as contents: the payload is appended behind the whole image (shim.ld)
shim.bin: shim.elf
	$(OBJCOPY) --set-section-flags .bss=alloc,load,contents -O binary $< $@
	@size=$$(wc -c < $@ | tr -d ' '); end=$$(($$($(LLVM)/llvm-nm $< | awk '/ __image_end$$/ { print "0x" $$1 }'))); \
	  [ "$$size" -eq "$$end" ] || { echo "shim.bin is $$size bytes, __image_end is $$end"; rm -f $@; exit 1; }; \
	  echo "shim.bin: $$size bytes"

fdt-test: fdt-test.c fdt.c memmap.c fdt.h memmap.h
	$(HOSTCC) -O1 -Wall -Wextra -I. fdt-test.c fdt.c memmap.c -o $@

tests/%.dtb: tests/%.dts
	$(DTC) -q -I dts -O dtb -o $@ $<

test: fdt-test tests/frankel-like.dtb
	./fdt-test tests/frankel-like.dtb

clean:
	rm -f *.o shim.elf shim.bin fdt-test tests/*.dtb

.PHONY: all test clean
