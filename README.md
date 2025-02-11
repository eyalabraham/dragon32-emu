# Dragon 32 computer emulator

[This project](https://sites.google.com/site/eyalabraham/dragon-32-computer) implements the software and the hardware needed to emulate a [Dragon 32 computer](https://en.wikipedia.org/wiki/Dragon_32/64). This repository provides both the Linux OS and Bare Metal versions for the Raspberry Pi (RPi) Zero with some external peripherals. The Dragon was my first computer when home/personal computers started to emerge in the mid 80s, and it is also one of the simplest to emulate. Other emulators exist, [including XROR](http://www.6809.org.uk/xroar/), but I decided to build my own as another exercise in RPi bare-metal programming.

> Linux version was tested on ```2021-12-02-raspios-buster-armhf-lite``` OS release. Raspberry Pi OS ```raspios-bullseye``` was found to not support 8 bit-per-pixel graphics using Linux Frame Buffer.

## Resources

- [The DRAGON Archives](https://worldofdragon.org/index.php?title=Main_Page) resources and software.
- [DRAGON Data archives](http://www.dragondata.co.uk/index.html) hardware schematics.
- [Inside the Dragon](http://www.dragondata.co.uk/Publications/InsideTheDragon.pdf) by Duncan Smeed and Ian Sommerville.
- [Dragon DOS patches, Dragon User magazine issues May/June/July 1985](https://colorcomputerarchive.com/repo/Documents/Magazines/Dragon%20User/). Reference "DRAGONDOS" in Dragon User [article index](https://archive.worldofdragon.org/images/7/7e/DragonUserIndex.pdf).
- Computers based on 6809 [CoCo Coding](https://sites.google.com/a/aaronwolfe.com/cococoding/home)
- [RPi BCM2835 GPIOs](https://elinux.org/RPi_BCM2835_GPIOs)
- [C library for Broadcom BCM 2835](https://www.airspayce.com/mikem/bcm2835/)

## Design

The CPU module is ```cpu.c```, the memory module is ```mem.c```. The IO module is implemented as call-back functions hooked through the memory module to selected memory addresses, and emulate the response of a memory mapper IO device. The IO module call-backs implement the various IO device in the emulated computer. This design is flexible enough to allow the definition of any computer configuration, memory and IO, built around an MC6809 CPU.  
  
The emulator will also be implemented with hardware: keyboard interface (PS2 through the Pi's SPI), audio DAC, and an analog comparator for a successive approximation ADC that will reproduce sound and support an **original** (circa 1984) Dragon Computer joystick.  

ASCII art depiction of the system for the RPi bare metal implementation:

```
  +-------------------------------------+                  |
  | Monitor (via Auxiliary UART)        |                  |
  |  Logger                             |                  |
  |  Monitor CLI                        |                  |
  +-------------------------------------+                  |
     |          |                |                 Software emulation
  +-----+  +----------+  +----------------------------+    |
  | CPU |--|   MEM    |--|          IO                |    |
  +-----+  +----------+  |                            |    |
                |        |                            |    |
           +----------+  |       |      |      |      |    |
           | Graphics |  | SAM   | PIA  | Disk | Tape |    |
           | xlate    |--| VGD   |      |      |      |    |
           +----------+  +-------+-------------+------+    |
                |                    |                   ------
           +----------+          +------+                  |
           | RPi      |          | RPi  |                  |
           | frame    |          | GPIO |               RPi HW
           | buff     |          |      |                  |
           +----------+          +------+                  |
                 |                   |                   ------
           +----------+      +---------------+             |
           | VGA      |      | Devices:      |             |
           | Monitor  |      |  PS2 Keyboard |         External HW
           +----------+      |  Joystick     |             |
                             |  Audio DAC    |             |
                             |  SD card      |             |
                             +---------------+             |
```

## Schematics

Schematics of external emulator hardware (KiCAD drawing)[https://github.com/eyalabraham/schematics/tree/master/dragon].

## Implementation

This repository contains all the intermediate implementation steps and tags them for easy retrieval. Each step builds on the functionality of its predecessors and maintains backward compatibility. The latest release is listed first:

- Release tag TBD.

### Stand alone CPU vs Dragon Computer emulation

The ```mem.c``` and ```cpu.c``` modules are all that are required for a basic MC6809E CPU emulator. The implementation of function ```cpu_run()``` in module ```cpu.c``` manages the execution of MC6809E code in both single step and continuous execution. Each time the function is called another CPU command is emulated, with CPU state available for inspection when single-stepping.

The method of repeatedly calling ```cpu_run()``` allow interruption for single stepping, break-point detection, and insertion of external events such as CPU interrupts.

To implement the full Dragon computer emulation, the ```cpu_run()``` function is called from an endless loop and IO device call-backs are implemented to carry out IO device activities.

### MC6809E CPU module

The Dragon computers where based on Motorola's [MC6806E](https://en.wikipedia.org/wiki/Motorola_6809) CPUs. The 6809 is an 8-bit microprocessor with some neat 16-bit features. The CPU module emulates the full set of CPU opcodes.

- The CPU module ```cpu.c``` interfaces with the memory module for reading opcode or data and writing to memory locations or to memory-mapped IO devices through the ```mem.c``` module
- Includes an API for external controls:
  - External interrupt emulation
  - CPU halt command
  - CPU Reset
  - CPU state and registers

### Memory module

The Dragon computer supported a maximum of 64K Bytes of memory. The memory map was managed by the SAM chip and divided into RAM, ROM, expansion ROM (cartridge), and memory mapped IO address spaces. In its basic state the memory module emulates 64K Bytes of RAM that can be accessed with the ```mem_read()``` and ```mem_write()``` API calls. Memory address ranges can be configured with special attributes that change their behavior into ROM or memory mapped IO addresses:

- ```mem_define_rom()``` will define a memory address range as read-only after which a ```mem_write()``` call would trigger a debug exception.
- ```mem_define_io()``` will define a memory address range as a memory mapped IO device and will register an IO device handler that will be called when a read or write calls are directed to addresses in the defined range.
- ```mem_load()``` will load a memory range with data copied from an input buffer.
- ```mem_init()``` will initialize memory.
  
#### Memory module data structures

```
typedef enum
{
    MEM_TYPE_RAM,
    MEM_TYPE_ROM,
    MEM_TYPE_IO,
} memory_flag_t;

typedef struct
{
    uint8_t data_byte;
    memory_flag_t memory_type;
    io_handler_callback io_handler;
} memory_t;
```

When the CPU emulation module reads a memory location is uses the ```mem_read()``` call that returns the contents of the memory address passed with the call. For a memory write using ```mem_write()``` call the following logic is applied:

1. Check if address is in range 0x0000 to 0xffff. If not flag exception and return with no action
2. Check memory location against MEM_FLAG_ROM flag. If memory location is ROM return with no action
3. Write data to memory location.

For both ```mem_read()``` and ```mem_write()``` check memory location against MEM_FLAG_IO flag. If flag is set, invoke the callback with the accessed address, the data (if a write operation) and a read/write flag. This will give the IO callback the context it needs to emulate the IO behind the memory address.

### IO emulation

The MC6809E CPU in the Dragon computer uses memory mapped IO devices. During initialization the emulation registers device callback functions that implement the IO devices' functionality. The callbacks are registered against memory address ranges associated with the device using the ```mem_define_io()``` call. The callbacks are invoked when reads or writes are issued to memory locations registered to IO devices. The ```dragon.c```, ```mon09.c``` and ```basic09.c``` computer emulation modules use IO callbacks to emulate the SAM, VDG, MC6821 PIA and MC6850 ACIA etc.  
  
The code in the call-backs redirect the IO request to the appropriate Raspberry Pi GPIO pin and function. The Dragon 32 emulation uses the following external GPIO:

| Function                    | RPi model B | RPi Zero / ZeroW |
|-----------------------------|-------------|------------------|
| Analog multiplexer select-0 | GPIO-02     | GPIO-02          |
| Analog multiplexer select-1 | GPIO-03     | GPIO-03          |
| RPi timing test point       | GPIO-04     | GPIO-04          |
| Emulator reset              | na          | GPIO-05          |
| Analog comparator input     | GPIO-07     | GPIO-07          |
| Right joystick "fire"       | GPIO-08     | GPIO-08          |
| AVR ATtiny85 keyboard MISO  | GPIO-09     | GPIO-09          |
| AVR ATtiny85 keyboard MOSI  | GPIO-10     | GPIO-10          |
| AVR ATtiny85 keyboard SCLK  | GPIO-11     | GPIO-11          |
| Serial TxD (future)         | GPIO-14     | GPIO-14          |
| Serial RxD (future)         | GPIO-15     | GPIO-15          |
| SD card CE                  | na          | GPIO-16          |
| AVR ATtiny85 reset          | GPIO-17     | GPIO-17          |
| SD card MISO                | na          | GPIO-19          |
| SD card MOSI                | na          | GPIO-20          |
| SD card SCLK                | na          | GPIO-21          |
| DAC bit.0                   | GPIO-22     | GPIO-22          |
| DAC bit.1                   | GPIO-23     | GPIO-23          |
| DAC bit.2                   | GPIO-24     | GPIO-24          |
| DAC bit.3                   | GPIO-25     | GPIO-25          |
| DAC bit.4                   | GPIO-18     | GPIO-26 (1)      |
| DAC bit.5                   | GPIO-27     | GPIO-27          |

Notes:
- (1) Changed on the Zero/ZeroW in order to form a contiguous bit order in the GPIO register.

#### SN74LS783/MC6883 Synchronous Address Multiplexer (SAM)

The [SAM chip](https://cdn.hackaday.io/files/1685367210644224/datasheet-MC6883_SAM.pdf) in the Dragon computer is responsible for IO address decoding, dynamic RAM memory refresh, and video memory address generation for the Video Display Generator (VDG) chip. Of these three functions, only the last one requires implementation. Since the SAM chip does not generate video, the address scanning is implemented in the VDG module by the ```vdg_render()``` function. The SAM device emulation only transfers offset address and video mode settings to the VDG module.

#### MC6847 Video Display Generator (VDG)

The VDG is Motorola's [MC6847](https://en.wikipedia.org/wiki/Motorola_6847) video chip. Since the VDG's video memory is part of the 64K Bytes of the CPU's memory map, then writes to that region are reflected into the RPi's video frame buffer by the IO handler of the VDG. The handler will adapt the writes to the RPi frame buffer based on the VDG/SAM modes for text or graphics. The Dragon computer video display emulation is implemented in the VDG module by the ```vdg_render()``` function, by accessing the Raspberry Pi Frame Buffer.

#### Casette tape emulation

The emulator implements a memory write trap on the 'CasLastSine' memory address. This memory address is used by a Drafgon 32 ROM routine that outputs a byte to tape. This method captures every data byte output to tape creating a CAS file formated image that can then be loaded back when needed. The implementation has two parts, the tape data load part is implemented in the ```pia.c``` module and the data save part in the ```tape.c``` module.

#### WD2797 floppy disk controller

WD2797 floppy disk controller and Dragon DOS ROM provides full emulation for using Dragon Dos formatted disk images loaded on SD card. Disk image loader supports the .VDK disk image format and loads disks using the loader sub-program accessible by escaping the emulation using the keyboard F1 key.  
The emulation supports a single drive. Single side, 40 tracks with 18 sectors per track, and a track size of 256 bytes.  

resources:  
- WD2797 floppy disk controller data sheet
- Dragon DOS programmer's guide, Grosvenor Software 1985
- Dragon DOS cartridge schematics
- Dragon DOS source code and ROM iamges https://github.com/prime6809/DragonDOS
- https://worldofdragon.org/index.php?title=Tape%5CDisk_Preservation#JVC.2FDSK_File_Format

#### 6821 parallel IO (PIA)

The Dragon computer's IO was provided by two MC6821 Peripheral Interface Adapters (PIAs).

##### Keyboard

The keyboard interface uses an ATtiny85 AVR coded with a PS2 to SPI interface. It implements a PS2 keyboard interface and an SPI serial interface. The AVR connects with the Raspberry Pi's SPI. The code configures the keyboard, accepts scan codes, converts the AT scan codes to ASCII make/break codes for the [Dragon 32 emulation](https://github.com/eyalabraham/dragon) running on the Raspberry Pi.
The AVR buffers the key codes in a small FIFO buffer, and the emulation periodically reads the buffer through the SPI interface.

```
 +-----+               +-----+            +-------+
 |     |               |     |            |       |
 |     +----[ MOSI>----+     |            |       |
 |     |               |     |            |       |
 |     +----< MISO]----+     +--< Data >--+ Level |
 | RPi |               | AVR |            | shift +---> PS2 keyboard
 |     +----[ SCL >----+     +--< CLK ]---+       |
 |     |               |     |            |       |
 |     +----[ RST >----+     |            |       |
 |     |               |     |            |       |
 +-----+               +-----+            +-------+
```

##### Audio

The Dragon's sound system is built around a simple 6-bit Digital to Analog (DAC). The DAC is a common [resistor ladder](https://en.wikipedia.org/wiki/Resistor_ladder) that is driven by a 6-bit MC4050 buffer. The Dragon emulator uses a similar setup with an 8-bit TTL buffer (74244) and similar resistor values. The GPIO pins of a Raspberry Pi B do not provide a contiguous set of output bits, so bit position translation is implemented.

```
 DAC    RPi B        (RPi Zero)
                                           +-------+
 bit.0  GPIO22 P1.15 (GPIO22 P1.15 )  >----+       +-[330K]-+
 bit.1  GPIO23 P1.16 (GPIO23 P1.16 )  >----+       +-[150K]-+
 bit.2  GPIO24 P1.18 (GPIO24 P1.18 )  >----+ 74244 +-[ 82K]-+
 bit.3  GPIO25 P1.22 (GPIO25 P1.22 )  >----+       +-[ 39K]-+
 bit.4  GPIO18 P1.12 (GPIO26 P1.37 )  >----+       +-[ 20K]-+
 bit.5  GPIO27 P1.13 (GPIO27 P1.13 )  >----+       +-[ 10K]-+---[Analog out>
                                           +-------+
```

In the Dragon computer the audio multiplexer is controlled by PIA0-CA2 and CB2, with PA1-CB2 controlling the audio source inhibit line. The CD4052 user in this emulator is different from the 4529 device used in the original computer and some changes in the emulation call-back are implemented to account for the difference. The changes reduce the number of supported joysticks to one with only the right joystick, and only two audio sources: DAC, and one open source for future use.

##### Joystick

The external hardware provides connectivity for the right joystick. The emulation software supports only one joystick. The external hardware is built with an analog multiplexer (CD4052) that routes the joystick output voltages to a comparator. The comparator works in conjunction with the DAC and the Dragon software to convert the analog joystick position to a number range between 0 and 63. The analog multiplexer is controlled by GPIO pins that represent PIA0-CA2 and PIA1-CB2 control lines, using low order select bit and the inhibit line instead of the high order select bit.

##### Field Sync IRQ

In the Dragon computer, the system generates an IRQ interrupt at the frame synchronization (FS) rate of 50 or 60Hz. The FS signal is routed through PIA0-CB1 (control register B-side) and generates an IRQ signal. Resetting the interrupt request by reading data register PIA0 B-side.

### Software loader

The software loader/manager interfaces with an SD card that holds Dragon 32 ROM cartridge images, VDK diskette images, tape images, and CAS files. The loader/manager can be escaped into from the emulation using the F1 key. Within the loader one can brows ROM, VDK, tape, and CAS files to load and run on the Dragon 32 emulator.

This functionality is available only on RPi Zero/W and uses an SD card interface connected to the auxiliary SPI interface (SPI1).

ROM code files are loaded as-is into the Dragon's ROM cartridge memory address space. No auto start is provided, but the BASIC EXEC vector is modified to point to 0xC000, so a simple EXEC from the BASIC prompt will start the ROM code.

VDK file are diskette image file. Selecting a VDK file "mounts" the diskette and DragonDOS commands mange it as if a diskette drive is connected to the Dragon 32 computer.

Tape image files act as tales loaded on an external tape recorder system. You can use CSAVE/M, CLOAD/M, SKIPF commands to save load and scan/skip files ona tape. This file type behaves exactly like a tape cassette storing data sequentially in the tape image file, so care must be taken not to overwrite files.

CAS files are digital images of old-style tape content and not memory images. More on [CAS file formats here](https://retrocomputing.stackexchange.com/questions/150/what-format-is-used-for-coco-cassette-tapes/153#153), and [Dragon 32 CAS format here](https://archive.worldofdragon.org/index.php?title=Tape%5CDisk_Preservation#CAS_File_Format). A cassette file can be mounted by the loader (like loading a cassette into a tape player), and then use the BASIC CLOAD or CLOADM commands to do the reading.

### Emulator main loop performance improvements

The main loop of the emulator is responsible for five tasks: execute CPU machine code from program memory, check state of reset button, check state of F1 function key for emulation escape, render video memory to RPi frame buffer, and generate VSYNC IRQ at 50Hz.  

Originally, the emulation produced distorted audio that I attributed to the average 3mSec pause at a 50Hz rate of the frame buffer updates done by vdg_render(). After measuring the emulation loop I discovered that the loop takes ~12uSec to complete, but the cpu_run() function completes at an average of 1uSec. This means that the emulator is running at a slow 300KHz equivalent CPU, and since audio is directly produced by CPU writes to the DAC this produced distorted audio. Most of the 12uSec interval is spent by vdg_render() and pia_vsync_irq() that were timed at about 4.8uSec each due to calls to clock() to retrieve system clock tick count.  

The change removed all calls to clock() and the 50Hz execution trigger of vdg_render() and pia_vsync_irq() is brute-forced by a simple counting cycles. Since each cpu_run() completes at an average time of 1uSec, adding the extra functions now extends the emulation loop to about 1.5uSec. The MC6809E CPU executes machine code utilizing an average of 3 to 4 clock cycles, so the emulation is now equivalent to about 3x faster than a real CPU, and a slow down empty loop is added to waste time and extend the loop up to about 4uSec total.  

Video is refreshed approximately every 20mSec (50Hz). The average 3mSec pause in CPU execution every 20mSec adds a 50Hz undertone that is largely filtered by the high-pass at the audio DAC output.  

The new emulator loop includes CPU execution (line 95), dead-time padding (line 99), test reset button state (lines 101 to 120), emulation escape (lines 122 to 124), vdg_render() and pia_vsync_irq() timing to match 50Hz (lines 126 to 131).  

```
 91    for (;;)
 92    {
 93        rpi_testpoint_on();
 94
 95        cpu_run();
 96
 97        rpi_testpoint_off();
 98
 99        for ( i = 0; i < CPU_TIME_WASTE; i++);
100
101        switch ( get_reset_state(LONG_RESET_DELAY) )
102        {
103            case 0:
104                cpu_reset(0);
105                break;
106
107            case 2:
108                /* Cold start flag set to value that is not 0x55
109                 */
110                mem_write(0x71, 0);
111                dbg_printf("Force cold restart.\n");
112                /* no break */
113
114            case 1:
115                cpu_reset(1);
116                break;
117
118            default:
119                dbg_printf("main(): unknown reset state.\n");
120        }
121
122        emulator_escape_code = pia_function_key();
123        if ( emulator_escape_code == ESCAPE_LOADER )
124            loader();
125
126        vdg_render_cycles++;
127        if ( vdg_render_cycles == VDG_RENDER_CYCLES )
128        {
129            vdg_render();
130            pia_vsync_irq();
131            vdg_render_cycles = 0;
132        }
133    }
```

### TODOs

- System reset (reset function for all peripherals, link to cpu_reset() etc.)
- Serial console for monitoring execution state
- Dragon sound sources: single bit in io_handler_pia1_pb() pia.c module

### Known problems

- Fire button "stuck" at push state (check "Donkey King" game)

## Files

```
.
├── bin
│   └── <binary-output>
├── boot
│   └── <bare-metal-boot-file>
├── doc
│   ├── DragonDOS-info.txt
│   └── dragon.pdf
├── include
│   ├── dragon
│   │   ├── ddos10.h
│   │   ├── ddos10p.h
│   │   ├── dragon.h
│   │   ├── font.h
│   │   └── semigraph.h
│   ├── rpi-bm
│   │   ├── auxuart.h
│   │   ├── bcm2835.h
│   │   ├── gpio.h
│   │   ├── irq.h
│   │   ├── mailbox.h
│   │   ├── spi0.h
│   │   ├── spi1.h
│   │   └── timer.h
│   ├── rpi-linux
│   │   ├── bcm2835.h
│   │   ├── spiaux.h
│   │   └── uart.h
│   ├── errors.h
│   ├── fat32.h
│   ├── loader.h
│   ├── mc6809e.h
│   ├── mem.h
│   ├── pia.h
│   ├── printf_config.h
│   ├── printf.h
│   ├── rpi.h
│   ├── config.h
│   ├── cpu.h
│   ├── dbgmsg.h
│   ├── disk.h
│   ├── tape.h
│   ├── sam.h
│   ├── sd.h
│   ├── trace.h
│   └── vdg.h
├── rpi-bm
│   ├── auxuart.c
│   ├── environment.mk
│   ├── gpio.c
│   ├── irq.c
│   ├── irq_util.S
│   ├── mailbox.c
│   ├── Makefile
│   ├── rpibm.c
│   ├── spi0.c
│   ├── spi1.c
│   ├── start.S
│   └── timer.c
├── rpi-linux
│   ├── Makefile
│   ├── rpi.c
│   ├── spiaux.c
│   └── uart.c
├── LICENSE.md
├── README.md
├── loader.c
├── Makefile
├── mem.c
├── pia.c
├── printf.c
├── cpu.c
├── dbgmsg.c
├── disk.c
├── tape.c
├── dragon.c
├── fat32.c
├── sam.c
├── sd.c
├── trace.c
└── vdg.c


```
