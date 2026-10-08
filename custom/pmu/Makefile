######################################
# target
######################################
TARGET = pmu_ne102

######################################
# NE102 PMU (N32L403KBQ7) firmware
# Based on the Nations.N32L40x_Library.2.3.0 PWR/STANDBY GCC example.
# Build: make            (arm-none-eabi-gcc must be in PATH, or pass GCC_PATH=<bin dir>)
# Local toolchain example (GNU Arm 13.3 bundled with STM32CubeIDE):
#   make GCC_PATH="E:\STM32CubeIDE_1.18.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.win32_1.0.0.202411081344\tools\bin"
######################################

DEBUG = 1
OPT = -Og

BUILD_DIR = build

######################################
# Official library (vendored subset)
######################################
LIB = lib/Nations.N32L40x_Library.2.3.0

######################################
# source
######################################
C_SOURCES = \
src/main.c \
src/n32l40x_it.c \
$(LIB)/CMSIS/device/system_n32l40x.c \
$(wildcard $(LIB)/n32l40x_std_periph_driver/src/*.c)

ASM_SOURCES = \
$(LIB)/CMSIS/device/startup/startup_n32l40x_gcc.s

#######################################
# binaries
#######################################
PREFIX = arm-none-eabi-
ifdef GCC_PATH
CC = $(GCC_PATH)/$(PREFIX)gcc
AS = $(GCC_PATH)/$(PREFIX)gcc -x assembler-with-cpp
CP = $(GCC_PATH)/$(PREFIX)objcopy
SZ = $(GCC_PATH)/$(PREFIX)size
else
CC = $(PREFIX)gcc
AS = $(PREFIX)gcc -x assembler-with-cpp
CP = $(PREFIX)objcopy
SZ = $(PREFIX)size
endif
HEX = $(CP) -O ihex
BIN = $(CP) -O binary -S

#######################################
# CFLAGS
#######################################
CPU = -mcpu=cortex-m4
FPU = -mfpu=fpv4-sp-d16
FLOAT-ABI = -mfloat-abi=soft
MCU = $(CPU) -mthumb $(FPU) $(FLOAT-ABI)

AS_DEFS =
C_DEFS = -DUSE_STDPERIPH_DRIVER -Dn32l40x
AS_INCLUDES =
C_INCLUDES = \
-Iinc \
-I$(LIB)/n32l40x_std_periph_driver/inc \
-I$(LIB)/CMSIS/device \
-I$(LIB)/CMSIS/core

ASFLAGS = $(MCU) $(AS_DEFS) $(AS_INCLUDES) $(OPT) -Wall -fdata-sections -ffunction-sections
CFLAGS += $(MCU) $(C_DEFS) $(C_INCLUDES) $(OPT) -Wall -fdata-sections -ffunction-sections
ifeq ($(DEBUG), 1)
CFLAGS += -g -gdwarf-2
endif
CFLAGS += -MMD -MP -MF"$(@:%.o=%.d)"

#######################################
# LDFLAGS
#######################################
# NE102-specific linker script: N32L403KB = 128K Flash + 24K RAM
# (the official n32l40x_flash.ld declares 32K RAM, too large for this part;
# see the note inside ld/)
LDSCRIPT = ld/n32l40x_flash_kb.ld

LIBS = -lc -lm -lnosys
LIBDIR =
LDFLAGS = $(MCU) -specs=nano.specs -T$(LDSCRIPT) $(LIBDIR) $(LIBS) -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref -Wl,--gc-sections

#######################################
# targets
#######################################
all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin

OBJECTS = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_SOURCES:.s=.o)))
vpath %.s $(sort $(dir $(ASM_SOURCES)))

$(BUILD_DIR)/%.o: %.c Makefile | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -Wa,-a,-ad,-alms=$(BUILD_DIR)/$(notdir $(<:.c=.lst)) $< -o $@

$(BUILD_DIR)/%.o: %.s Makefile | $(BUILD_DIR)
	$(AS) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) Makefile
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(SZ) $@

$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(HEX) $< $@

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(BIN) $< $@

$(BUILD_DIR):
	mkdir $@

# J-Link download (Windows; adjust the JLink.exe path to the actual install)
JLINK ?= D:\Program Files\SEGGER\JLink_V962\JLink.exe
flash: all
	"$(JLINK)" -Device N32L403KB -If SWD -Speed 4000 -AutoConnect 1 -CommandFile jlink/flash.jlink

clean:
	rm -rf $(BUILD_DIR)

-include $(wildcard $(BUILD_DIR)/*.d)

# *** EOF ***
