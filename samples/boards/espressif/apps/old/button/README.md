# Description

This is a simple example of button testing using the ESP32-S3 SoC and Zephyr RTOS.

# requirements

- GPIO pin 0


# build and run

## build
```
west build -b esp32s3_devkitc/esp32s3/procpu .\samples\boards\espressif\apps\button\
```

## flash
> COM14 is the COM port of the ESP32-S3
```
west flash --esp-device COM14
```

## monitor
```
west espressif monitor --port COM14 
```

## output
```bash
(.venv) PS zephyr (main)> west espressif monitor --port COM14
--- WARNING: GDB cannot open serial ports accessed as COMx
--- Using \\.\COM14 instead...
--- idf_monitor on \\.\COM14 115200 ---
--- Quit: Ctrl+] | Menu: Ctrl+T | Help: Ctrl+T followed by Ctrl+H ---
ESP-ROM:esp32s3-20210327
Build:Mar 27 2021
rst:0x1 (POWERON),boot:0xb (SPI_FAST_FLASH_BOOT)
SPIWP:0xee
mode:DIO, clock div:1
load:0x3fc8e6e0,len:0x1d14
load:0x40374000,len:0xa6cc
SHA-256 comparison failed:
Calculated: dd59bf252e28b01904f2eaa6ee52f7aa67a13ab2320b9ec926cd6f75473fa1e7
Expected: 00000000e03b0000000000000000000000000000000000000000000000000000
Attempting to boot anyway...
entry 0x403794e4
I (58) soc_init: ESP Simple boot
I (58) soc_init: compile time Nov 11 2025 11:23:48
W (59) soc_init: Unicore bootloader
I (59) soc_init: chip revision: v0.2
I (61) flash_init: Boot SPI Speed : 80MHz
I (65) flash_init: SPI Mode       : DIO
I (68) flash_init: SPI Flash Size : 8MB
I (72) boot: DRAM       : lma=00000020h vma=3fc8e6e0h size=01d14h (  7444)
I (78) boot: IRAM       : lma=00001d3ch vma=40374000h size=0a6cch ( 42700)
I (84) boot: IRAM       : lma=0000c418h vma=00000000h size=03be0h ( 15328)
I (90) boot: IRAM       : lma=00010000h vma=42000000h size=041e8h ( 16872)
I (96) boot: IRAM       : lma=000141f0h vma=00000000h size=0be08h ( 48648)
I (102) boot: IRAM      : lma=00020000h vma=3c010000h size=01520h (  5408)
I (109) boot: IROM      : lma=00010000h vma=42000000h size=041E6h ( 16870) map
I (115) boot: DROM      : lma=00020000h vma=3c010000h size=01520h (  5408) map
I (133) boot: libc heap size 342 kB.
I (133) spi_flash: detected chip: boya
I (133) spi_flash: flash io: dio
W (133) spi_flash: Detected size(16384k) larger than the size in the binary image header(8192k). Using the size in the binary image header.
*** Booting Zephyr OS build v4.2.0-3648-g85bbf2632bd7 ***
Set up button at gpio@60004000 pin 0
Press the button
Button pressed at 1974705041
Button pressed at 3394116929
Button pressed at 4294012439
Button pressed at 584168758
Button pressed at 1151541022
Button pressed at 1854694879
Button pressed at 2392042126
Button pressed at 2938174279
Button pressed at 3989585410
```