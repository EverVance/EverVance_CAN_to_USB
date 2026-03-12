################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../source/FlashDev.c \
../source/FlashPrg.c 

S_SRCS += \
../source/checkblank.s 

C_DEPS += \
./source/FlashDev.d \
./source/FlashPrg.d 

OBJS += \
./source/FlashDev.o \
./source/FlashPrg.o \
./source/checkblank.o 


# Each subdirectory must supply rules for building sources it contributes
source/%.o: ../source/%.c source/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: MCU C Compiler'
	arm-none-eabi-gcc -D__REDLIB__ -D__MCUXPRESSO -D__USE_CMSIS -DNDEBUG -DCPU_MIMXRT1052DVL6B_cm7 -DCPU_MIMXRT1052DVL6B -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\iMXRT1050_QSPI\source" -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\iMXRT1050_QSPI\utilities" -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\iMXRT1050_QSPI\drivers" -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\iMXRT1050_QSPI\CMSIS" -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\iMXRT1050_QSPI\QSPIsource" -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\iMXRT1050_QSPI\board" -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\LPCXFlashDriverLib\inc" -Os -Wall -c -fmessage-length=0 -fno-builtin -ffunction-sections -fdata-sections -fsingle-precision-constant -fmacro-prefix-map="$(<D)/"= -mcpu=cortex-m7 -mthumb -D__REDLIB__ -fstack-usage -specs=redlib.specs -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.o)" -MT"$(@:%.o=%.d)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '

source/%.o: ../source/%.s source/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: MCU Assembler'
	arm-none-eabi-gcc -c -x assembler-with-cpp -D__REDLIB__ -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\iMXRT1050_QSPI\board" -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\iMXRT1050_QSPI\source" -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\iMXRT1050_QSPI\drivers" -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\iMXRT1050_QSPI\CMSIS" -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\iMXRT1050_QSPI\utilities" -I"F:\AS\NXP_Workspace\VBA_CAN\tools\linkserver\custom_driver\LPCXFlashDriverLib\inc" -g3 -gdwarf-4 -mcpu=cortex-m7 -mthumb -D__REDLIB__ -specs=redlib.specs -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


clean: clean-source

clean-source:
	-$(RM) ./source/FlashDev.d ./source/FlashDev.o ./source/FlashPrg.d ./source/FlashPrg.o ./source/checkblank.o

.PHONY: clean-source

