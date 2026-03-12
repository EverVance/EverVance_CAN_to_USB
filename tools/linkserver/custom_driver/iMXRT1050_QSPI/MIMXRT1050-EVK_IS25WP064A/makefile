################################################################################
# Automatically-generated file. Do not edit!
################################################################################

-include ../makefile.init

RM := rm -rf

# All of the sources participating in the build are defined here
-include sources.mk
-include utilities/subdir.mk
-include source/subdir.mk
-include drivers/subdir.mk
-include board/subdir.mk
-include QSPIsource/subdir.mk
-include CMSIS/subdir.mk
ifneq ($(MAKECMDGOALS),clean)
ifneq ($(strip $(C_DEPS)),)
-include $(C_DEPS)
endif
endif

-include ../makefile.defs

OPTIONAL_TOOL_DEPS := \
$(wildcard ../makefile.defs) \
$(wildcard ../makefile.init) \
$(wildcard ../makefile.targets) \


BUILD_ARTIFACT_NAME := iMXRT1050_QSPI
BUILD_ARTIFACT_EXTENSION := axf
BUILD_ARTIFACT_PREFIX :=
BUILD_ARTIFACT := $(BUILD_ARTIFACT_PREFIX)$(BUILD_ARTIFACT_NAME)$(if $(BUILD_ARTIFACT_EXTENSION),.$(BUILD_ARTIFACT_EXTENSION),)

# Add inputs and outputs from these tool invocations to the build variables 

# All Target
all:
	+@$(MAKE) --no-print-directory main-build && $(MAKE) --no-print-directory post-build

# Main-build Target
main-build: iMXRT1050_QSPI.axf

# Tool invocations
iMXRT1050_QSPI.axf: $(OBJS) $(USER_OBJS) makefile $(OPTIONAL_TOOL_DEPS)
	@echo 'Building target: $@'
	@echo 'Invoking: MCU Linker'
	arm-none-eabi-gcc -nostdlib -s -L"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\LPCXFlashDriverLib\Release_SectorHashing" -Xlinker -n -Xlinker -print-memory-usage -Xlinker --gc-sections -Xlinker -Map="iMXRT1050_QSPI.map" -mcpu=cortex-m7 -mthumb -T FlashDriver_32Kbuffer.ld -L ../linkscripts -o "iMXRT1050_QSPI.axf" $(OBJS) $(USER_OBJS) $(LIBS) -lLPCXFlashDriverLib
	@echo 'Finished building target: $@'
	@echo ' '

# Other Targets
clean:
	-$(RM) iMXRT1050_QSPI.axf
	-@echo ' '

post-build:
	-@echo 'Performing post-build steps'
	-arm-none-eabi-size "iMXRT1050_QSPI.axf" ; mkdir -p ../builds ; cp iMXRT1050_QSPI.axf ../builds/MIMXRT1050-EVK_IS25WP064A.cfx
	-@echo ' '

.PHONY: all clean dependents main-build post-build

-include ../makefile.targets
