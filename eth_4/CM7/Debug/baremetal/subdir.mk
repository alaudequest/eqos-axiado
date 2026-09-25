################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../baremetal/eqos.c \
../baremetal/eqos_port.c 

OBJS += \
./baremetal/eqos.o \
./baremetal/eqos_port.o 

C_DEPS += \
./baremetal/eqos.d \
./baremetal/eqos_port.d 


# Each subdirectory must supply rules for building sources it contributes
baremetal/%.o baremetal/%.su baremetal/%.cyclo: ../baremetal/%.c baremetal/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m7 -std=gnu11 -g3 -DDEBUG -DCORE_CM7 -DUSE_HAL_DRIVER -DSTM32H755xx -DUSE_PWR_DIRECT_SMPS_SUPPLY -DUSE_NUCLEO_64 -c -I../Core/Inc -I../../Drivers/STM32H7xx_HAL_Driver/Inc -I../../Drivers/STM32H7xx_HAL_Driver/Inc/Legacy -I../../Drivers/BSP/STM32H7xx_Nucleo -I../../Drivers/CMSIS/Device/ST/STM32H7xx/Include -I../../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-baremetal

clean-baremetal:
	-$(RM) ./baremetal/eqos.cyclo ./baremetal/eqos.d ./baremetal/eqos.o ./baremetal/eqos.su ./baremetal/eqos_port.cyclo ./baremetal/eqos_port.d ./baremetal/eqos_port.o ./baremetal/eqos_port.su

.PHONY: clean-baremetal

