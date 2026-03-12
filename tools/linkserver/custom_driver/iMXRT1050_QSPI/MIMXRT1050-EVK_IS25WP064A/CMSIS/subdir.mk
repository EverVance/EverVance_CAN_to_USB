################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../CMSIS/system_MIMXRT1052.c 

C_DEPS += \
./CMSIS/system_MIMXRT1052.d 

OBJS += \
./CMSIS/system_MIMXRT1052.o 


# Each subdirectory must supply rules for building sources it contributes
CMSIS/%.o: ../CMSIS/%.c CMSIS/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: MCU C Compiler'
	arm-none-eabi-gcc -D__REDLIB__ -D__MCUXPRESSO -D__USE_CMSIS -DNDEBUG -DCPU_MIMXRT1052DVL6B_cm7 -DCPU_MIMXRT1052DVL6B -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\iMXRT1050_QSPI\source" -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\iMXRT1050_QSPI\utilities" -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\iMXRT1050_QSPI\drivers" -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\iMXRT1050_QSPI\CMSIS" -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\iMXRT1050_QSPI\QSPIsource" -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\iMXRT1050_QSPI\board" -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\LPCXFlashDriverLib\inc" -Os -Wall -c -fmessage-length=0 -fno-builtin -ffunction-sections -fdata-sections -fsingle-precision-constant -fmacro-prefix-map="$(<D)/"= -mcpu=cortex-m7 -mthumb -D__REDLIB__ -fstack-usage -specs=redlib.specs -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.o)" -MT"$(@:%.o=%.d)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


clean: clean-CMSIS

clean-CMSIS:
	-$(RM) ./CMSIS/system_MIMXRT1052.d ./CMSIS/system_MIMXRT1052.o

.PHONY: clean-CMSIS

