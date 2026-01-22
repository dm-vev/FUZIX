cmake_minimum_required(VERSION 3.12)

# Pull in SDK (must be before project)
include(pico_sdk_import.cmake)

project(fuzix C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_BUILD_TYPE Debug)
set(PICO_COPY_TO_RAM 1)
option(TOTALMEM "specify how much TOTALMEM is present" 0)
option(ENABLE_PSRAM "Enable PicoCalc PSRAM (ramdisk + framebuffer)" ON)

add_compile_definitions(
		PICO_HEAP_SIZE=0x0
		PICO_NO_BINARY_INFO=1
		PICO_TIME_DEFAULT_ALARM_POOL_DISABLED
		PICO_PANIC_FUNCTION=fuzix_pico_panic
    TOTALMEM=${TOTALMEM}
		ENABLE_PSRAM=$<BOOL:${ENABLE_PSRAM}>
		)

pico_sdk_init()

include_directories(
	.
	../..
	../../dev
	../../include
	../../cpu-armm0
)

add_executable(fuzix
	devices.c
	devflash.c
	devsdspi.c
	devaudio_stream.c
	devaudio_pcm.c
	lcdspi.c
	i2ckbd.c
	devtty.c
	elf.c 
	main.c
	misc.c
	rawflash.c
	rawuart.c
	swapper.c
	tricks.S
	core1.c
	devgpio.c
	usbdescriptors.c
	pico_panic.c
	../../dev/blkdev.c
	../../dev/devrd.c
	../../dev/mbr.c
	../../dev/devsd_discard.c
	../../dev/devsd.c
	../../lib/dhara/error.c
	../../lib/dhara/journal.c
	../../lib/dhara/map.c
	../../lib/hexdump.c
	../../fatfs.c
	../../blk512.c
	../../devio.c
	../../devsys.c
	../../filesys.c
	../../inode.c
	../../kdata.c
	../../cpu-armm0/lowlevel-armm0.c
	../../mm/malloc.c
	../../mm.c
	../../mm/memalloc_none.c
	../../process.c
	../../start.c
	../../swap.c
	../../syscall_execelf32.c
	../../syscall_exec.c
	../../syscall_fs.c
	../../syscall_fs2.c
	../../syscall_fs3.c
	../../syscall_other.c
	../../syscall_proc.c
	../../timer.c
	../../tty.c
	../../usermem.c
	../../version.c
	../../dev/net/net_native.c
	../../syscall_net.c
	../../network.c
)

if (ENABLE_PSRAM)
	target_sources(fuzix PRIVATE
		devrd_psram.c
		devfb.c
		psram_spi.c
	)
	pico_generate_pio_header(fuzix ${CMAKE_CURRENT_LIST_DIR}/psram_spi.pio)
	target_compile_definitions(fuzix PRIVATE
		PSRAM_MUTEX
		PSRAM_PIN_CS=20
		PSRAM_PIN_SCK=21
		PSRAM_PIN_MOSI=2
		PSRAM_PIN_MISO=3
	)
else()
	target_sources(fuzix PRIVATE
		devrd_none.c
		devfb_none.c
	)
endif()

target_link_libraries(fuzix
	pico_stdlib
	pico_multicore
	hardware_flash
	hardware_dma
	hardware_pwm
	hardware_pio
	hardware_spi
	hardware_i2c
	tinyusb_device
)

pico_set_float_implementation(fuzix none)
pico_set_double_implementation(fuzix none)
pico_add_extra_outputs(fuzix)
