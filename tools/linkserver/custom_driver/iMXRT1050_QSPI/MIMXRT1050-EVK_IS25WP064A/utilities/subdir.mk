################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../utilities/fsl_assert.c \
../utilities/fsl_debug_console.c \
../utilities/fsl_io.c \
../utilities/fsl_log.c \
../utilities/fsl_str.c 

C_DEPS += \
./utilities/fsl_assert.d \
./utilities/fsl_debug_console.d \
./utilities/fsl_io.d \
./utilities/fsl_log.d \
./utilities/fsl_str.d 

OBJS += \
./utilities/fsl_assert.o \
./utilities/fsl_debug_console.o \
./utilities/fsl_io.o \
./utilities/fsl_log.o \
./utilities/fsl_str.o 


# Each subdirectory must supply rules for building sources it contributes
utilities/%.o: ../utilities/%.c utilities/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: MCU C Compiler'
	arm-none-eabi-gcc -D__REDLIB__ -D__MCUXPRESSO -D__USE_CMSIS -DNDEBUG -DCPU_MIMXRT1052DVL6B_cm7 -DCPU_MIMXRT1052DVL6B -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\iMXRT1050_QSPI\source" -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\iMXRT1050_QSPI\utilities" -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\iMXRT1050_QSPI\drivers" -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\iMXRT1050_QSPI\CMSIS" -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\iMXRT1050_QSPI\QSPIsource" -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\iMXRT1050_QSPI\board" -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\LPCXFlashDriverLib\inc" -Os -Wall -c -fmessage-length=0 -fno-builtin -ffunction-sections -fdata-sections -fsingle-precision-constant -fmacro-prefix-map="$(<D)/"= -mcpu=cortex-m7 -mthumb -D__REDLIB__ -fstack-usage -specs=redlib.specs -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.o)" -MT"$(@:%.o=%.d)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


clean: clean-utilities

clean-utilities:
	-$(RM) ./utilities/fsl_assert.d ./utilities/fsl_assert.o ./utilities/fsl_debug_console.d ./utilities/fsl_debug_console.o ./utilities/fsl_io.d ./utilities/fsl_io.o ./utilities/fsl_log.d ./utilities/fsl_log.o ./utilities/fsl_str.d ./utilities/fsl_str.o

.PHONY: clean-utilities

