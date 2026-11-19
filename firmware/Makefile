##############################################################################
# UAV-DAQ firmware - command-line build for STM32F401RE (arm-none-eabi-gcc)
#
# STATUS: UNTESTED. The supported build is STM32CubeIDE (see README.md in this
# directory). This Makefile only works after STM32CubeMX has regenerated the
# vendor code from "STM32 Telemetry.ioc", because the repository does not
# contain:
#     Drivers/                      STM32F4 HAL + CMSIS
#     Middlewares/Third_Party/FatFs FatFs R0.12c
#     FATFS/App, FATFS/Target       fatfs.c/.h, ffconf.h, user_diskio.h
#     STM32F401RETX_FLASH.ld        linker script
#
#   make            build build/uav-daq.elf / .hex / .bin
#   make flash      program with st-flash
#   make host-test  run the PC unit tests (needs only a host gcc)
##############################################################################

TARGET    = uav-daq
BUILD_DIR = build

PREFIX  ?= arm-none-eabi-
CC       = $(PREFIX)gcc
OBJCOPY  = $(PREFIX)objcopy
SIZE     = $(PREFIX)size

HAL_DIR   = Drivers/STM32F4xx_HAL_Driver
CMSIS_DIR = Drivers/CMSIS
FATFS_MW  = Middlewares/Third_Party/FatFs/src
LDSCRIPT  = STM32F401RETX_FLASH.ld

VENDOR_REQUIRED = $(HAL_DIR)/Src $(CMSIS_DIR)/Include $(FATFS_MW)/ff.c FATFS/App/fatfs.c $(LDSCRIPT)

# Application sources that ARE in this repository
C_SOURCES = \
  Core/Src/main.c \
  Core/Src/telemetry.c \
  Core/Src/stm32f4xx_it.c \
  Core/Src/stm32f4xx_hal_msp.c \
  Core/Src/system_stm32f4xx.c \
  Core/Src/syscalls.c \
  Core/Src/sysmem.c \
  Core/Src/ssd1306.c \
  Core/Src/ssd1306_fonts.c \
  drivers/user_diskio.c

# Vendor sources produced by STM32CubeMX (drivers/user_diskio.c replaces the
# empty FATFS/Target/user_diskio.c template, which is therefore not listed)
C_SOURCES += \
  $(filter-out %_template.c,$(wildcard $(HAL_DIR)/Src/*.c)) \
  $(FATFS_MW)/ff.c \
  $(FATFS_MW)/diskio.c \
  $(FATFS_MW)/ff_gen_drv.c \
  $(wildcard $(FATFS_MW)/option/syscall.c) \
  FATFS/App/fatfs.c

ASM_SOURCES = Core/Startup/startup_stm32f401retx.s

INCLUDES = \
  -ICore/Inc \
  -Idrivers \
  -IFATFS/App \
  -IFATFS/Target \
  -I$(FATFS_MW) \
  -I$(HAL_DIR)/Inc \
  -I$(HAL_DIR)/Inc/Legacy \
  -I$(CMSIS_DIR)/Device/ST/STM32F4xx/Include \
  -I$(CMSIS_DIR)/Include

MCU     = -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
DEFS    = -DUSE_HAL_DRIVER -DSTM32F401xE
CFLAGS  = $(MCU) $(DEFS) $(INCLUDES) -std=gnu11 -Og -g3 -Wall -ffunction-sections -fdata-sections
# -u _printf_float: snprintf("%.2f") needs float support in newlib-nano
LDFLAGS = $(MCU) -T$(LDSCRIPT) --specs=nano.specs --specs=nosys.specs -u _printf_float \
          -Wl,--gc-sections -Wl,-Map=$(BUILD_DIR)/$(TARGET).map -lc -lm

OBJECTS = $(addprefix $(BUILD_DIR)/,$(C_SOURCES:.c=.o) $(ASM_SOURCES:.s=.o))

all: check-vendor $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin

check-vendor:
	@missing=""; for f in $(VENDOR_REQUIRED); do [ -e "$$f" ] || missing="$$missing $$f"; done; \
	if [ -n "$$missing" ]; then \
	  echo "error: vendor files are not in this repository:$$missing"; \
	  echo "       Regenerate them from 'STM32 Telemetry.ioc' with STM32CubeMX first -"; \
	  echo "       see firmware/README.md. ('make host-test' works without them.)"; \
	  exit 1; \
	fi

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) -c $(MCU) -x assembler-with-cpp $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(SIZE) $@

$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf
	$(OBJCOPY) -O ihex $< $@

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf
	$(OBJCOPY) -O binary $< $@

flash: all
	st-flash write $(BUILD_DIR)/$(TARGET).bin 0x8000000

host-test:
	$(MAKE) -C tests

clean:
	rm -rf $(BUILD_DIR)
	$(MAKE) -C tests clean

.PHONY: all check-vendor flash host-test clean
