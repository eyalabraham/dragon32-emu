###############################################################################
#
# Raspberry Pi bare metal Makefile for Dragon 32 emulator on RPi
#
###############################################################################

# Remove existing implicit rules
.SUFFIXES:

#------------------------------------------------------------------------------
# Defines
#   TOOLDIR     - Tool chain directory
#   BINDIR      - Binary output directory
#   CCFLAGS     - C compiler flags
#   ASFLAGS     - Assembler flags
#   LDFLAGS     - Linker flags
#   CC          - Compiler
#   AS          - Assembler
#   LD          - Linker
#   OBJCOPY     - Object code conversion
#   ARMARCH     - Processor architecture
#------------------------------------------------------------------------------
include environment.mk

PIMODEL ?= RPI1

#------------------------------------------------------------------------------
# Define RPi model
#------------------------------------------------------------------------------
CCFLAGS += -D$(PIMODEL) -DRPI_BARE_METAL=1

#------------------------------------------------------------------------------------
# Dependencies
#------------------------------------------------------------------------------------
OBJDRAGON = start.o ../dragon.o \
            ../mem.o ../cpu.o \
            ../sam.o ../pia.o ../vdg.o ../disk.o \
            ../printf.o ../dbgmsg.o ../sd.o ../fat32.o ../loader.o \
            rpibm.o gpio.o auxuart.o timer.o spi0.o spi1.o mailbox.o irq.o irq_util.o

#------------------------------------------------------------------------------
# New make patterns
#------------------------------------------------------------------------------
%.o: %.c
	$(CC) $(CCFLAGS) -I $(INCDIR) -c $< -o $(OUTDIR)/$(notdir $@)

%.o: %.S
	$(AS) $(ASFLAGS) $< -o $(OUTDIR)/$(notdir $@)

#------------------------------------------------------------------------------
# Build all targets
#------------------------------------------------------------------------------
all: dragon

dragon: $(OBJDRAGON)
#	$(LD) $(LDFLAGS) -L $(LIBDIR1) -L $(LIBDIR2) -o $@.elf $? -lgpio -lprintf -lgcc -lg_nano
	cd $(OUTDIR) && echo "** Entering "$(OUTDIR); \
	$(LD) $(LDFLAGS) -L $(LIBDIR1) -L $(LIBDIR2) -o $(OUTDIR)/$@.elf $(notdir $?) -lgcc -lg_nano
	$(OBJCOPY) $(OUTDIR)/$@.elf -O binary $(BOOTDIR)/$@.img
	cp $(BOOTDIR)/$@.img $(BOOTDIR)/kernel.img
	cd $(BASE) && echo "** Exiting "$(OUTDIR); \

#------------------------------------------------------------------------------
# Cleanup
#------------------------------------------------------------------------------

.PHONY: clean

clean:
	rm -f *.bak
	rm -f $(OUTDIR)/*.o
	rm -f $(OUTDIR)/*.map
	rm -f $(OUTDIR)/*.elf
	rm -f $(BOOTDIR)/*.img
