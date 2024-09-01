#####################################################################################
#
#  This make file is for compiling the Dragon 32 emulator
#  on Raspberry Pi with Raspbian Linux OS.
#  Make will rsync files and run compilation on the RPi.
#
#  Use:
#    clean      - clean environment
#    all        - build all outputs
#
#####################################################################################

# Remove existing implicit rules
.SUFFIXES:

#------------------------------------------------------------------------------
# Define RPi model
#------------------------------------------------------------------------------
CCFLAGS += -DRPI_BARE_METAL=0

#------------------------------------------------------------------------------------
# project directories
#------------------------------------------------------------------------------------
INCDIR = ../include
SRCDIR = ..
OUTDIR = ../bin

#------------------------------------------------------------------------------------
# build tool and options
#------------------------------------------------------------------------------------
CC = gcc

#OPT = -Wall -L/usr/local/lib -lbcm2835 -I $(INCDIR)
#OPT = -Wall -g -I $(INCDIR)
OPT = -Wall -I $(INCDIR)

#------------------------------------------------------------------------------------
# dependencies
#------------------------------------------------------------------------------------
DEPS = mem.h cpu.h mc6809e.h rpi.h sam.h pia.h vdg.h disk.h printf.h spiaux.h rpi-linux/uart.h sdfat32.h loader.h
OBJDRAGON = ../dragon.o \
            ../mem.o ../cpu.o ../sam.o ../pia.o ../vdg.o ../disk.o \
			../printf.o ../trace.o spiaux.o ../sd.o ../fat32.o rpi.o ../loader.o ../dbgmsg.o

_DEPS = $(patsubst %,$(INCDIR)/%,$(DEPS))

#------------------------------------------------------------------------------------
# build all targets
#------------------------------------------------------------------------------------
%.o: %.c
	$(CC) $(OPT) $(CCFLAGS) -c $< -o $(OUTDIR)/$(notdir $@)

all: sync

dragon: $(OBJDRAGON)
	$(CC) -L/usr/local/lib $(addprefix $(OUTDIR)/,$(?F)) -lbcm2835 -o $(OUTDIR)/$@

#------------------------------------------------------------------------------------
# rsync files and run remote 'make'
# requires ssh key setup to avoid using password authentication
#------------------------------------------------------------------------------------
sync:
	rsync -vrh $(SRCDIR)/*  pi@dragon:/home/pi/dragon
	ssh pi@dragon "cd /home/pi/dragon/rpi-linux && make dragon"

rclean:
	ssh pi@dragon "cd /home/pi/dragon && make clean"

#------------------------------------------------------------------------------------
# cleanup
#------------------------------------------------------------------------------------
.PHONY: clean

clean:
	rm -f dragon
	rm -f emu09
	rm -f mon09
	rm -f basic09
	rm -f intr09
	rm -f profile
	rm -f *.o
	rm -f *.bak

