//*****************************************************************************
// i.MXRT1050 M7 Microcontroller Startup code for use with MCUXpresso IDE
//
// Version : 161020
//*****************************************************************************
//
// Copyright 2016, 2018, 2020, 2021 NXP
// All rights reserved.
//
// SPDX-License-Identifier: BSD-3-Clause
//*****************************************************************************
#if defined (__cplusplus)
#ifdef __REDLIB__
#error Redlib does not support C++
#else
//*****************************************************************************
//
// The entry point for the C++ library startup
//
//*****************************************************************************
extern "C" {
    extern void __libc_init_array(void);
}
#endif
#endif

#define WEAK __attribute__ ((weak))
#define WEAK_AV __attribute__ ((weak, section(".after_vectors")))
#define ALIAS(f) __attribute__ ((weak, alias (#f)))

//*****************************************************************************
#if defined (__cplusplus)
extern "C" {
#endif

//*****************************************************************************
// Declaration of external SystemInit function
extern void SystemInit(void);

//*****************************************************************************
//
// Forward declaration of the default handlers. These are aliased.
// When the application defines a handler (with the same name), this will
// automatically take precedence over these weak definitions
//
//*****************************************************************************
     void ResetISR(void);
WEAK void NMI_Handler(void);
WEAK void HardFault_Handler(void);
WEAK void MemManage_Handler(void);
WEAK void BusFault_Handler(void);
WEAK void UsageFault_Handler(void);
WEAK void SVC_Handler(void);
WEAK void DebugMon_Handler(void);
WEAK void PendSV_Handler(void);
WEAK void SysTick_Handler(void);
WEAK void IntDefaultHandler(void);

WEAK void WDT_IRQHandler(void);
WEAK void RTC_IRQHandler(void);
WEAK void TIM0_IRQHandler(void);
WEAK void TIM2_IRQHandler(void);
WEAK void MCIA_IRQHandler(void);
WEAK void MCIB_IRQHandler(void);
WEAK void UART0_IRQHandler(void);
WEAK void UART1_IRQHandler(void);
WEAK void UART2_IRQHandler(void);
WEAK void UART3_IRQHandler(void);
WEAK void UART4_IRQHandler(void);
WEAK void AACI_IRQHandler(void);
WEAK void CLCD_IRQHandler(void);
WEAK void ENET_IRQHandler(void);
WEAK void USBDC_IRQHandler(void);
WEAK void USBHC_IRQHandler(void);
WEAK void CHLCD_IRQHandler(void);
WEAK void FLEXRAY_IRQHandler(void);
WEAK void CAN_IRQHandler(void);
WEAK void LIN_IRQHandler(void);
WEAK void I2C_IRQHandler(void);
WEAK void CPU_CLCD_IRQHandler(void);
WEAK void SPI_IRQHandler(void);

//*****************************************************************************
//
// Forward declaration of the specific IRQ handlers. These are aliased
// to the IntDefaultHandler, which is a 'forever' loop. When the application
// defines a handler (with the same name), this will automatically take
// precedence over these (void);definitions
//
//*****************************************************************************
// Example...
// void WDT_IRQHandler(void) ALIAS(IntDefaultHandler);

//*****************************************************************************
//
// The entry point for the application.
// __main() is the entry point for Redlib based applications
// main() is the entry point for Newlib based applications
//
//*****************************************************************************
#if defined (__REDLIB__)
extern void __main(void);
#endif
extern int main(void);
//*****************************************************************************
//
// External declaration for the pointer to the stack top from the Linker Script
//
//*****************************************************************************
extern void _vStackTop(void);

//*****************************************************************************
#if defined (__cplusplus)
} // extern "C"
#endif
//*****************************************************************************
//
// The vector table.
// This relies on the linker script to place at correct location in memory.
//
//*****************************************************************************
extern void (* const g_pfnVectors[])(void);
__attribute__ ((used,section(".isr_vector")))
void (* const g_pfnVectors[])(void) = {
    // Core Level - CM7
    &_vStackTop,                        // The initial stack pointer
    ResetISR,                           // The reset handler
    NMI_Handler,                        // The NMI handler
    HardFault_Handler,                  // The hard fault handler
    MemManage_Handler,                  // The MPU fault handler
    BusFault_Handler,                   // The bus fault handler
    UsageFault_Handler,                 // The usage fault handler
    0,                                  // Reserved
    0,                                  // Reserved
    0,                                  // Reserved
    0,                                  // Reserved
    SVC_Handler,                        // SVCall handler
    DebugMon_Handler,                   // Debug monitor handler
    0,                                  // Reserved
    PendSV_Handler,                     // The PendSV handler
    SysTick_Handler,                    // The SysTick handler

    // MCU specific handlers
    WDT_IRQHandler,						//  0 - Windowed watchdog timer
    RTC_IRQHandler,
    TIM0_IRQHandler,
    TIM2_IRQHandler,
    MCIA_IRQHandler,
    MCIB_IRQHandler,
    UART0_IRQHandler,
    UART1_IRQHandler,
    UART2_IRQHandler,
    UART3_IRQHandler,
    UART4_IRQHandler,
    AACI_IRQHandler,
    CLCD_IRQHandler,
    ENET_IRQHandler,
    USBDC_IRQHandler,
    USBHC_IRQHandler,
    CHLCD_IRQHandler,
    FLEXRAY_IRQHandler,
    CAN_IRQHandler,
    LIN_IRQHandler,
    I2C_IRQHandler,
    CPU_CLCD_IRQHandler,
    SPI_IRQHandler
};

//*****************************************************************************
// Functions to carry out the initialization of RW and BSS data sections. These
// are written as separate functions rather than being inlined within the
// ResetISR() function in order to cope with MCUs with multiple banks of
// memory.
//*****************************************************************************
__attribute__ ((section(".after_vectors")))
void data_init(unsigned int romstart, unsigned int start, unsigned int len) {
    unsigned int *pulDest = (unsigned int*) start;
    unsigned int *pulSrc = (unsigned int*) romstart;
    unsigned int loop;
    for (loop = 0; loop < len; loop = loop + 4)
        *pulDest++ = *pulSrc++;
}

__attribute__ ((section(".after_vectors")))
void bss_init(unsigned int start, unsigned int len) {
    unsigned int *pulDest = (unsigned int*) start;
    unsigned int loop;
    for (loop = 0; loop < len; loop = loop + 4)
        *pulDest++ = 0;
}

//*****************************************************************************
// The following symbols are constructs generated by the linker, indicating
// the location of various points in the "Global Section Table". This table is
// created by the linker via the Code Red managed linker script mechanism. It
// contains the load address, execution address and length of each RW data
// section and the execution and length of each BSS (zero initialized) section.
//*****************************************************************************
extern unsigned int __data_section_table;
extern unsigned int __data_section_table_end;
extern unsigned int __bss_section_table;
extern unsigned int __bss_section_table_end;

//*****************************************************************************
// Reset entry point for your code.
// Sets up a simple runtime environment and initializes the C/C++
// library.
//*****************************************************************************
__attribute__ ((section(".after_vectors")))
void
ResetISR(void)
{
    //
    // Copy the data sections from flash to SRAM.
    //
    unsigned int LoadAddr, ExeAddr, SectionLen;
    unsigned int *SectionTableAddr;

    // Load base address of Global Section Table
    SectionTableAddr = &__data_section_table;

    // Copy the data sections from flash to SRAM.
    while (SectionTableAddr < &__data_section_table_end) {
        LoadAddr = *SectionTableAddr++;
        ExeAddr = *SectionTableAddr++;
        SectionLen = *SectionTableAddr++;
        data_init(LoadAddr, ExeAddr, SectionLen);
    }
    // At this point, SectionTableAddr = &__bss_section_table;
    // Zero fill the bss segment
    while (SectionTableAddr < &__bss_section_table_end) {
        ExeAddr = *SectionTableAddr++;
        SectionLen = *SectionTableAddr++;
        bss_init(ExeAddr, SectionLen);
    }

#if defined (__VFP_FP__) && !defined (__SOFTFP__)
/*
 * Code to enable the Cortex-M4 FPU only included
 * if appropriate build options have been selected.
 * Code taken from Section 7.1, Cortex-M4 TRM (DDI0439C)
 */
    // Read CPACR (located at address 0xE000ED88)
    // Set bits 20-23 to enable CP10 and CP11 coprocessors
    // Write back the modified value to the CPACR
    asm volatile ("LDR.W R0, =0xE000ED88\n\t"
                  "LDR R1, [R0]\n\t"
                  "ORR R1, R1, #(0xF << 20)\n\t"
                  "STR R1, [R0]\n\t"
                  "DSB\n\t"
                  "ISB");
#endif // (__VFP_FP__) && !(__SOFTFP__)

    // Check to see if we are running the code from a non-zero
    // address (eg RAM, external flash), in which case we need
    // to modify the VTOR register to tell the CPU that the
    // vector table is located at a non-0x0 address.
    unsigned int * pSCB_VTOR = (unsigned int *) 0xE000ED08;
    if ((unsigned int *)g_pfnVectors!=(unsigned int *) 0x00000000) {
        // CMSIS : SCB->VTOR = <address of vector table>
        *pSCB_VTOR = (unsigned int)g_pfnVectors;
    }

    SystemInit();

#if defined (__cplusplus)
    //
    // Call C++ library initialisation
    //
    __libc_init_array();
#endif

#if defined (__REDLIB__)
    // Call the Redlib library, which in turn calls main()
    __main() ;
#else
    main();
#endif

    //
    // main() shouldn't return, but if it does, we'll just enter an infinite loop
    //
    while (1) {
        ;
    }
}

//*****************************************************************************
// Default exception handlers. Override the ones here by defining your own
// handler routines in your application code.
//*****************************************************************************
WEAK_AV void NMI_Handler(void)
{
	while(1);
}

WEAK_AV void HardFault_Handler(void)
{
	while(1);
}

WEAK_AV void MemManage_Handler(void)
{
	while(1);
}

WEAK_AV void BusFault_Handler(void)
{
	while(1);
}

WEAK_AV void UsageFault_Handler(void)
{
	while(1);
}

WEAK_AV void SVC_Handler(void)
{
	while(1);
}

WEAK_AV void DebugMon_Handler(void)
{
	while(1);
}

WEAK_AV void PendSV_Handler(void)
{
	while(1);
}

WEAK_AV void SysTick_Handler(void)
{
	while(1);
}

//*****************************************************************************
//
// Processor ends up here if an unexpected interrupt occurs or a specific
// handler is not present in the application code.
//
//*****************************************************************************
__attribute__ ((section(".after_vectors")))
void IntDefaultHandler(void)
{
	while(1);
}

//*****************************************************************************
//
// MCU Interrupt Handlers
//
//*****************************************************************************

WEAK void WDT_IRQHandler(void)
{
	while(1);
}

WEAK void RTC_IRQHandler(void)
{
	while(1);
}

WEAK void TIM0_IRQHandler(void)
{
	while(1);
}

WEAK void TIM2_IRQHandler(void)
{
	while(1);
}

WEAK void MCIA_IRQHandler(void)
{
	while(1);
}

WEAK void MCIB_IRQHandler(void)
{
	while(1);
}

WEAK void UART0_IRQHandler(void)
{
	while(1);
}

WEAK void UART1_IRQHandler(void)
{
	while(1);
}

WEAK void UART2_IRQHandler(void)
{
	while(1);
}

WEAK void UART3_IRQHandler(void)
{
	while(1);
}

WEAK void UART4_IRQHandler(void)
{
	while(1);
}

WEAK void AACI_IRQHandler(void)
{
	while(1);
}

WEAK void CLCD_IRQHandler(void)
{
	while(1);
}

WEAK void ENET_IRQHandler(void)
{
	while(1);
}

WEAK void USBDC_IRQHandler(void)
{
	while(1);
}

WEAK void USBHC_IRQHandler(void)
{
	while(1);
}

WEAK void CHLCD_IRQHandler(void)
{
	while(1);
}

WEAK void FLEXRAY_IRQHandler(void)
{
	while(1);
}

WEAK void CAN_IRQHandler(void)
{
	while(1);
}

WEAK void LIN_IRQHandler(void)
{
	while(1);
}

WEAK void I2C_IRQHandler(void)
{
	while(1);
}

WEAK void CPU_CLCD_IRQHandler(void)
{
	while(1);
}

WEAK void SPI_IRQHandler(void)
{
	while(1);
}


