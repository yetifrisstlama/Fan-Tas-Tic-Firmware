#include <stdint.h>
#include <stdbool.h>
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "inc/hw_timer.h"
#include "inc/hw_gpio.h"
#include "inc/hw_ints.h"
#include "inc/hw_pwm.h"
// TivaWare includes
#include "driverlib/rom.h"
#include "driverlib/timer.h"
#include "driverlib/pwm.h"
#include "driverlib/pin_map.h"
#include "driverlib/gpio.h"
#include "driverlib/sysctl.h"
//USB stuff
#include "usblib/usblib.h"
#include "usblib/usbcdc.h"
#include "usblib/device/usbdevice.h"
#include "usblib/device/usbdcdc.h"
#include "drivers/usb_serial_structs.h"
//My stuff
#include "my_uartstdio.h"
#include "myTasks.h"
#include "io_manager.h"
#include "mySpi.h"

TaskHandle_t hUSBCommandParser = NULL;
volatile bool g_bFeedWatchdog = true;
volatile bool g_bWatchdogIsTripped = false;

//-------------------------------------------------------------------------
// Helper functions to Setup a hardware counter for simple cycle counting
// To see how fast crtical parts of the code can run
//-------------------------------------------------------------------------
void configureTimer() {
    ROM_SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER1);        // Enable Timer 1 Clock
    ROM_SysCtlPeripheralReset(SYSCTL_PERIPH_TIMER1);
    ROM_TimerConfigure(TIMER1_BASE, TIMER_CFG_ONE_SHOT_UP); // Configure Timer Operation as one shot up counting
//    ROM_TimerConfigure(TIMER1_BASE, TIMER_CFG_PERIODIC_UP ); // For freertos stats
    ROM_TimerEnable(TIMER1_BASE, TIMER_A);                   // Start Timer 1A
    stopTimer();
}
void startTimer() {
    ROM_TimerEnable(TIMER1_BASE, TIMER_A); // Start Timer 1A
}
uint32_t stopTimer() {
//  Returns the number of cycles since startTimer()
    uint32_t timerValue;
    ROM_TimerDisable(TIMER1_BASE, TIMER_A); // Stop Timer 1A
    timerValue = ROM_TimerValueGet(TIMER1_BASE, TIMER_A);
    ROM_TimerLoadSet(TIMER1_BASE, TIMER_A, 0xFFFFFFFF);
    HWREG(TIMER1_BASE + TIMER_O_TAV) = 0;
    return (timerValue);
}
uint32_t getTimer(){
    static uint32_t timerValue=0;
    timerValue += stopTimer()>>8;
    startTimer();
    return timerValue;
}

void vApplicationStackOverflowHook( TaskHandle_t xTask, char *pcTaskName ){
    DISABLE_SOLENOIDS();
    configASSERT(0);
}

void vApplicationMallocFailedHook( void ){
    DISABLE_SOLENOIDS();
    configASSERT(0);
}

void ledOut(uint8_t ledVal){
    // 0 = off, 1 = blue, 2 = green, 3 = blue & green
    // red led shared with spi bus!
    ROM_GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_3|GPIO_PIN_2, ledVal<<2);
}

//-------------------------------------------------------------------------
// Hardware initialization
//-------------------------------------------------------------------------
void initGpio(){
    ROM_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);
    ROM_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOB);
    ROM_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOC);
    ROM_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOD);
    ROM_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOE);
    ROM_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF);
    // Unlock NMI to use PD7 as normal GPIO (not needed asPD7 is configured as GPIO input by default)
    HWREG(GPIO_PORTD_BASE + GPIO_O_LOCK) = GPIO_LOCK_KEY;
    HWREG(GPIO_PORTD_BASE + GPIO_O_CR) |= 0x80;
    HWREG(GPIO_PORTD_BASE + GPIO_O_AFSEL) &= ~0x80;
    HWREG(GPIO_PORTD_BASE + GPIO_O_DEN) |= 0x80;
    HWREG(GPIO_PORTD_BASE + GPIO_O_LOCK) = 0;
    // Configure Switch matrix inputs
    ROM_GPIOPinTypeGPIOInput(GPIO_PORTE_BASE, GPIO_PIN_3 | GPIO_PIN_2 | GPIO_PIN_1);
    ROM_GPIOPinTypeGPIOInput(GPIO_PORTC_BASE, GPIO_PIN_7 | GPIO_PIN_6);
    ROM_GPIOPinTypeGPIOInput(GPIO_PORTD_BASE, GPIO_PIN_7 | GPIO_PIN_6);
    ROM_GPIOPinTypeGPIOInput(GPIO_PORTF_BASE, GPIO_PIN_4);
    // Configure GPIO Pins for UART mode (usb debug terminal).
    ROM_GPIOPinConfigure(GPIO_PA0_U0RX);
    ROM_GPIOPinConfigure(GPIO_PA1_U0TX);
    ROM_GPIOPinTypeUART(GPIO_PORTA_BASE, GPIO_PIN_0 | GPIO_PIN_1);
    // Configure DEVICE USB pins
    ROM_GPIOPinTypeUSBAnalog(GPIO_PORTD_BASE, GPIO_PIN_5 | GPIO_PIN_4);
    // Init switch matrix shift register driving outputs
    ROM_GPIODirModeSet(GPIO_PORTB_BASE, GPIO_PIN_1 | GPIO_PIN_0, GPIO_DIR_MODE_OUT);
    ROM_GPIOPadConfigSet(GPIO_PORTB_BASE, GPIO_PIN_1 | GPIO_PIN_0, GPIO_STRENGTH_12MA, GPIO_PIN_TYPE_OD);
    // Enable the GPIO pins for the LED (green|blue|red).
    ROM_GPIOPinTypeGPIOOutput(GPIO_PORTF_BASE, GPIO_PIN_3|GPIO_PIN_2|GPIO_PIN_1);
    // Enable the GPIO pins for the 24V enbale (PE0).
    ROM_GPIOPinTypeGPIOOutput(GPIO_PORTE_BASE, GPIO_PIN_0);
    ROM_GPIODirModeSet( GPIO_PORTE_BASE, GPIO_PIN_0, GPIO_DIR_MODE_OUT );
    ROM_GPIOPadConfigSet( GPIO_PORTE_BASE, GPIO_PIN_0, GPIO_STRENGTH_12MA, GPIO_PIN_TYPE_STD );
    DISABLE_SOLENOIDS();
}

void myPWMGenConfigure(uint32_t ui32Base, uint32_t ui32Gen){
    // Enable and configure one of the 4 PWM generators (2 PWM outputs each)
    // Compute the generator's base address.
    ui32Gen += ui32Base;
    // Change the global configuration of the generator (PWMnCTL).
    // Local sync. COunting down, enabled
    //HWREG(ui32Gen + PWM_O_X_CTL) = PWM_X_CTL_DBFALLUPD_LS | PWM_X_CTL_DBRISEUPD_LS | PWM_X_CTL_DBCTLUPD_LS | PWM_X_CTL_GENBUPD_LS | PWM_X_CTL_GENAUPD_LS;
    // set the 2 x PWM signal low on reload and high on compare match
    HWREG(ui32Gen + PWM_O_X_GENA) = PWM_X_GENA_ACTCMPAD_ONE | PWM_X_GENA_ACTLOAD_ZERO;
    HWREG(ui32Gen + PWM_O_X_GENB) = PWM_X_GENB_ACTCMPBD_ONE | PWM_X_GENB_ACTLOAD_ZERO;
    // set the reload register (sets PWM frequency)
    HWREG(ui32Gen + PWM_O_X_LOAD)  = MAX_PWM + 1;
    // set pwm value to 0 (output never set high)
    HWREG(ui32Gen + PWM_O_X_CMPA) = 0;
    HWREG(ui32Gen + PWM_O_X_CMPB) = 0;
    // Enable it
    HWREG(ui32Gen + PWM_O_X_CTL) |= PWM_X_CTL_ENABLE;
}

void initPWM(){
    //--------------------------------
    // Out    M0PWM2, 3,   6,   7
    // On     PB4,    PB5, PC4, PC5
    // With   PWM_GEN_1    PWM_GEN_3
    //--------------------------------
    // Enable PWM hardware
    ROM_SysCtlPeripheralEnable( SYSCTL_PERIPH_PWM0 );
    ROM_SysCtlPeripheralReset(  SYSCTL_PERIPH_PWM0 );
    ROM_SysCtlPWMClockSet( SYSCTL_PWMDIV_1 );           //PWM counter runs at 80 MHz
    // Setup output pins
    ROM_GPIOPinConfigure(GPIO_PB4_M0PWM2);
    ROM_GPIOPinConfigure(GPIO_PB5_M0PWM3);
    ROM_GPIOPinConfigure(GPIO_PC4_M0PWM6);
    ROM_GPIOPinConfigure(GPIO_PC5_M0PWM7);
    // Setup PIN type
    ROM_GPIOPinTypePWM(  GPIO_PORTB_BASE, GPIO_PIN_4 );
    ROM_GPIOPinTypePWM(  GPIO_PORTB_BASE, GPIO_PIN_5 );
    ROM_GPIOPinTypePWM(  GPIO_PORTC_BASE, GPIO_PIN_4 );
    ROM_GPIOPinTypePWM(  GPIO_PORTC_BASE, GPIO_PIN_5 );
    // Configure the PWM generators for count down mode
    // Set the period to 50 kHz. Max PWM value = 1600
    myPWMGenConfigure( PWM0_BASE, PWM_GEN_1 );
    myPWMGenConfigure( PWM0_BASE, PWM_GEN_3 );
    // Invert the outputs.
    //HWREG( PWM0_BASE + PWM_O_INVERT ) = PWM_INVERT_PWM2INV | PWM_INVERT_PWM3INV | PWM_INVERT_PWM6INV | PWM_INVERT_PWM7INV;
    // Enable the outputs.
    HWREG( PWM0_BASE + PWM_O_ENABLE ) = PWM_ENABLE_PWM2EN | PWM_ENABLE_PWM3EN | PWM_ENABLE_PWM6EN | PWM_ENABLE_PWM7EN;
}

void setPwm( uint8_t channel, uint16_t pwmValue ){
    // channel 0 ... 3,  pwmValue 0 ... MAX_PWM
    uint32_t ui32Gen;
//    if( pwmValue > MAX_PWM ){
//        return;
//    }
    switch( channel ){
    case 0:
        ui32Gen = PWM_GEN_1 + PWM_O_X_CMPA;     //PWM2
        break;
    case 1:
        ui32Gen = PWM_GEN_1 + PWM_O_X_CMPB;     //PWM3
        break;
    case 2:
        ui32Gen = PWM_GEN_3 + PWM_O_X_CMPA;     //PWM6
        break;
    case 3:
        ui32Gen = PWM_GEN_3 + PWM_O_X_CMPB;     //PWM7
        break;
    default:
        return;
    }
    HWREG( PWM0_BASE + ui32Gen ) = pwmValue;
}

void WatchdogIntHandler(void)
{
    // If we have been told to stop feeding the watchdog, return immediately
    // without clearing the interrupt.  This will cause the system to reset
    // next time the watchdog interrupt fires.
    if(g_bFeedWatchdog && !g_bWatchdogIsTripped) {
        // Clear the watchdog interrupt.
        ROM_WatchdogIntClear(WATCHDOG0_BASE);
        g_bFeedWatchdog = false;
    } else {
        // Panic shutdown in 1s !!!!
        DISABLE_SOLENOIDS();
        g_bWatchdogIsTripped = true;
        ROM_GPIOPinWrite(GPIO_PORTF_BASE, 0x0E, 1 << 1);  // red LED
        ROM_IntDisable(INT_WATCHDOG);
    }
}

void initWdt(void)
{
    g_bFeedWatchdog = true;

    ROM_SysCtlPeripheralEnable(SYSCTL_PERIPH_WDOG0);

    if(ROM_WatchdogLockState(WATCHDOG0_BASE) == true)
        ROM_WatchdogUnlock(WATCHDOG0_BASE);

    // Set the period of the watchdog timer to 1 s.
    ROM_WatchdogReloadSet(WATCHDOG0_BASE, ROM_SysCtlClockGet());

    // Enable interrupt generation from the watchdog timer.
    ROM_IntEnable(INT_WATCHDOG);

    // Enable reset generation from the watchdog timer.
    ROM_WatchdogResetEnable(WATCHDOG0_BASE);

    // Enable the watchdog timer.
    ROM_WatchdogEnable(WATCHDOG0_BASE);
}

//-------------------------------------------------------------------------
// Main function
//-------------------------------------------------------------------------
int main(void) {
    // run at 80 MHz from the PLL
    ROM_SysCtlClockSet(
        SYSCTL_SYSDIV_2_5 |
        SYSCTL_USE_PLL |
        SYSCTL_XTAL_16MHZ |
        SYSCTL_OSC_MAIN
    );
    initWdt();
    initGpio();
    // Set up the UART which is connected to the virtual debugging COM port
    UARTStdioConfig(0, 1152000, SYSTEM_CLOCK);
    // Enable lazy stacking for interrupt handlers.  This allows floating-point
    // instructions to be used within interrupt handlers, but at the expense of
    // extra stack usage.
    ROM_FPULazyStackingEnable();

    //-------------------------------------------------------------------------
    // Init USB virtual serial port for communication to the host PC runing MPF
    //-------------------------------------------------------------------------
    // Configure the required pins for USB operation.
    ROM_SysCtlPeripheralEnable(SYSCTL_PERIPH_USB0);
    // Initialize the transmit and receive buffers.
    USBBufferInit(&g_sTxBuffer);
    USBBufferInit(&g_sRxBuffer);
    // Set the USB stack mode to Device mode with VBUS monitoring.
    USBStackModeSet(0, eUSBModeForceDevice, 0);
    // Pass our device information to the USB library and place the device
    // on the bus.
    USBDCDCInit(0, &g_sCDCDevice);

    //-------------------------------------------------------------------------
    // Init I2C
    //-------------------------------------------------------------------------
    // Write 0 to all PCFs (in case there is relays)
    init_i2c_system(false);
    // Init debug HW timer for measuring processor cycles (%timeit)
    configureTimer();
    // Init 3 SPI channels for setting ws2811 LEDs
    spiSetup();
    // Init the 4 high speed PWM output channels
    initPWM();

    //-------------------------------------------------------------------------
    // The WS2811 LEDs will glitch if the SPI buffer has an underflow, hence
    // the SPI interrupt gets a higher priority
    // Highest Int priority = (0<<5)    (only upper 3 bits count)
    // Lowest Int priority  = (7<<5)
    //-------------------------------------------------------------------------
    ROM_IntPrioritySet(INT_USB0, (6<<5));     //USB = Low priority
    ROM_IntPrioritySet(INT_I2C0, (6<<5));     //I2C = Medium priority
    ROM_IntPrioritySet(INT_I2C1, (6<<5));
    ROM_IntPrioritySet(INT_I2C2, (6<<5));
    ROM_IntPrioritySet(INT_I2C3, (6<<5));
    ROM_IntPrioritySet(INT_UART0,(7<<5));     //Debug UART = Lowest priority
    ROM_IntPrioritySet(INT_SSI1, (5<<5));     //SPI  = High priority
    ROM_IntPrioritySet(INT_SSI2, (5<<5));
    ROM_IntPrioritySet(INT_SSI3, (5<<5));
    ROM_IntPrioritySet(INT_WATCHDOG, (7<<5));

    //-------------------------------------------------------------------------
    // Startup the FreeRTOS scheduler
    //-------------------------------------------------------------------------
    // Blink LED
    xTaskCreate(taskDemoLED, (const portCHAR *)"LEDr", configMINIMAL_STACK_SIZE, NULL, 1, NULL);

    // Handle reading / writing I2C bus, fast PWM and switch matrix
    xTaskCreate(task_pcf_io, (const portCHAR *)"IO", 128, NULL, 1, &hPcfInReader);

    // Create USB command parser task
    xTaskCreate(taskUsbCommandParser, (const portCHAR *)"Parser", 128, NULL, 1, &hUSBCommandParser);

    vTaskStartScheduler();  // This should never return!
    return 0;
}

//ASSERT() Error function failed ASSERTS() from driverlib/debug.h are executed in this function
void __error__(char *pcFilename, uint32_t ui32Line) {
    DISABLE_SOLENOIDS();
    ROM_GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_3|GPIO_PIN_2|GPIO_PIN_1, 2);
    while (1)
        ; // Place a breakpoint here to capture errors until logging routine is finished
}
