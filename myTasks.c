/*
 * tasks.c
 *
 *  Created on: Dec 22, 2015
 *      Author: michael
 */

//-----------------------------
//# Digital inputs / outputs
//-----------------------------
//At init, scan each i2c bus from 0x40 - 0x47 --> these are PCF8574s
//Keep a list of PC8574 addresses
//To use an PC8574 as input we have to write a 0xFF to it (to prevent the open drain output pulling it low)
//Otherwise outputs and inputs can be processed the same
//
// Switch states are stored as uint8_t array
// Byte Index AA = 0x00 - 0xFF
//  0 -  7: switch matrix
//  8 - 15: I2C GPIO expanders on I2C0
// 16 - 23: I2C GPIO expanders on I2C1
// 24 - 31: I2C GPIO expanders on I2C2
// 32 - 39: I2C GPIO expanders on I2C3
// 40 - 255:Reserved
//
//
// Calculating HW-adress from byte index: HW = AA*8 + B  --> HW = 12 bit = 0 ... 1023
// AA = byte index, B = pin index (0-7)
//
// Calculating byte index from HW-adress:
// AA = HW/8        B = HW%8
//
//
// AA-=8; AA<0:None(SM);
// Calculating I2Cch from AA:
// int(AA/8); AA>3:None(Res);
//
// Calculating I2Cadr from AA:
// (AA)%8 + 0x40
//
// Switch input HW - addresses:
//---------------------------------
// 0 - 63  = Switch matrix
// 64 - 71 = I2C[0][0] --> Solenoid driver on mainboard
// 72 - 79 = I2C[0][1] --> First external PCL GPIO extender on I2C channel 0
// 312-319 = I2C[3][7] --> 7th   external PCL GPIO extender on I2C channel 3

//
// Hardcoded solenoid drivers: 0x0800 - 0x0807
//
// Reading an output switch will readback its logical value. So all I2C extenders can be read in bulk
// I2C is fast eough to read all of them with 625 Hz repetition rate (the ones not connected will report 0xFF)
//
// There is the debounce routine running with 333 Hz, reading all switches (matrix and I2C) into an array
// * switch matrix, read the rows / columns
// * I2C reader, queries all port extenders   (runs in ISR background)
// Deboouncing: use a vertical counter algo.
//
// The quickrule task listens to switch cange events.
// It checks a local list of rules, which can lead to immediate actions, like coils firing and such.
//
// The serial reporter task listens to switch change events. Encodes them and reports them on the serial port
//
// There must be a command to report the current switch state (just send the complete uint8_t array)
//
// Command to pulse a coil with: tPulse, pulsePWM, holdPWM --> keep holdPWM state (which can be 0) forever
//
//-----------------------------
//# WS2811 RGB LED strings
//-----------------------------
// Raw data values are attached to serial command and just sent on one of the channels
// Queue to SPI send thread
// --> Update LEDs
//-----------------------------
//# I2C
//-----------------------------
// THere must be commands to do send / receive of custom bytes to custom addresses
// Queue to I2C custom sent thread. Mutex of I2C hardware!
//-----------------------------
//# Coonfigure commands
//-----------------------------
// Pulse a driver:
//    * pulse time  [ms]
//    * pulse power [pwm units]
//    * hold power  [pwm units]
//
// Configure switch:
//    * debouncing ticks (10-80 ms)?
//
// Configure QuickRule:
//  * quickRuleId (0-64)
//     * input switch ID
//  * trigger type flags ([0] enable/disable, [1] trigger on positive/negative edge, [2] disable ouput on release, [3] invert output, [4] apply now)
//    * post trigger hold-off time [ms]
//  * driver output ID
//  * pulse duration 0 - 100 [ms]
//    * pulse pwm [only for output ID 0-3 which are the pwm channels]
//    * hold pwm  [only for output ID 0-3 which are the pwm channels]
//
// Writing a 0 to an input switch will disable the readback and should be avoided
// Only write to the I2C addresses which have been specified as outputs!
//

#include <stdint.h>
#include <stdbool.h>
#include "driverlib/debug.h"
#include "drivers/pinout.h"
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "driverlib/i2c.h"
#include "sensorlib/i2cm_drv.h"
#include "driverlib/sysctl.h"

// FreeRTOS includes
#include "FreeRTOSConfig.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

// TivaWare includes
#include "utils/ustdlib.h"
#include "utils/uartstdio.h"
#include "utils/cmdline.h"
// USB stuff
#include "usblib/usblib.h"
#include "usblib/usbcdc.h"
#include "usblib/device/usbdevice.h"
#include "usblib/device/usbdcdc.h"
#include "usb_serial_structs.h"
#include "myTasks.h"
#include "i2cHandlerTask.h"

uint16_t strMyStrip(uint8_t *cmdString, uint16_t cmdLen) {
    //Remove \n, \r and make sure there is a \0 at the end
    uint8_t *pos = cmdString;
    uint16_t i;
    for (i = 0; i < cmdLen; i++) {
        if (*pos == '\n' || *pos == '\r' || *pos == '\0') {
            *pos = '\0';
            return (i);
        }
        pos++;
    }
    *pos = '\0';
    return (i);
}

void taskDemoLED(void *pvParameters) {
// Flash the LEDs on the launchpad
    UARTprintf("%22s: %s", "taskDemoLED()", "Started!\n");
    while (1) {
        // Turn on LED 1
        LEDWrite(0x0F, 0x01);
        vTaskDelay(300);
        // Turn on LED 2
        LEDWrite(0x0F, 0x02);
        vTaskDelay(300);
        // Turn on LED 3
        LEDWrite(0x0F, 0x04);
        vTaskDelay(300);
        // Turn on LED 4
        LEDWrite(0x0F, 0x08);
        vTaskDelay(300);
    }
}

void taskUsbCommandParser(void *pvParameters) {
    // Read data from USB serial and parse it
    UARTprintf("%22s: %s", "taskUsbCommandParser()", "Started!\n");
    static uint8_t charBuffer[CMD_PARSER_BUF_LEN];
//    uint32_t minStackSpace;
    uint16_t nCharsRead = 0;
    charBuffer[CMD_PARSER_BUF_LEN - 1] = '\0';
    while (1) {
        if (ulTaskNotifyTake( pdTRUE, portMAX_DELAY)) {    // Wait for receiving new serial data over USB
            while (USBBufferDataAvailable(&g_sRxBuffer)) {
                nCharsRead += USBBufferRead(&g_sRxBuffer,
                        &charBuffer[nCharsRead],
                        CMD_PARSER_BUF_LEN - nCharsRead);
                char lastChar = charBuffer[nCharsRead - 1];
                if (lastChar == '\n' || lastChar == '\r' || lastChar == '\0') {
                    nCharsRead = strMyStrip(charBuffer, nCharsRead);
                    int retVal = CmdLineProcess((char*) charBuffer);
                    switch (retVal) {
                    case CMDLINE_BAD_CMD:
                        UARTprintf("[CMDLINE_BAD_CMD] %s\n", charBuffer);
                        break;
                    case CMDLINE_INVALID_ARG:
                        UARTprintf("[CMDLINE_INVALID_ARG] %s\n", charBuffer);
                        break;
                    case CMDLINE_TOO_FEW_ARGS:
                        UARTprintf("[CMDLINE_TOO_FEW_ARGS] %s\n", charBuffer);
                        break;
                    case CMDLINE_TOO_MANY_ARGS:
                        UARTprintf("[CMDLINE_TOO_MANY_ARGS] %s\n", charBuffer);
                        break;
                    }
                    nCharsRead = 0;
                }
                ASSERT(nCharsRead < CMD_PARSER_BUF_LEN);// Check for buffer overflow due to too long command
            }
//            minStackSpace = uxTaskGetStackHighWaterMark( NULL );
//            UARTprintf("taskUsbCommandParser(): Min Stack Space = %d words\n", minStackSpace);
        }
    }
}

t_PCLOutputByte g_outWriterList[OUT_WRITER_LIST_LEN];

void setBcm(uint8_t *bcmBuffer, uint8_t pin, uint8_t pwmValue) {
    uint8_t j;
    taskENTER_CRITICAL();
    for (j = 0; j < N_BIT_PWM; j++) {
        HWREGBITB( bcmBuffer, pin ) = HWREGBITB(&pwmValue, j);
        bcmBuffer++;
    }
    taskEXIT_CRITICAL();
}

void handleBitRules(t_PCLOutputByte *outListPtr, uint8_t dt) {
    uint8_t i;
    t_BitModifyRules *bitRules = outListPtr->bitRules;
    for (i = 0; i <= 7; i++) {
        if (bitRules->tPulse > 0) {                        //Is the entry valid?
            bitRules->tPulse -= dt;                            //Apply dt timestep
            if (bitRules->tPulse <= 0) {             //Did the countdown expire?
                setBcm(outListPtr->bcmBuffer, i, bitRules->lowPWM);    //Then apply the pulse_low bcm pattern
            }
        }
        bitRules++;
    }
}

void taskPCLOutWriter(void *pvParameters) {
    // Dispatch I2C write commands to PCL GPIO extenders every 1 ms
    // Use binary code modulation for N bit PWM
    UARTprintf("%22s: %s", "taskPCLOutWriter()", "Started!\n");
    uint8_t i, j, lastTickCount = 0;
    uint8_t bcmCycleCounter = 0;    //Which bit to output
    uint32_t c = 0, ticks;
    TickType_t xLastWakeTime;
    t_PCLOutputByte *outListPtr = g_outWriterList;
//    -------------------------------------------------------
//    Init Data structure for caching the output values
//    -------------------------------------------------------
    for (i = 0; i < OUT_WRITER_LIST_LEN; i++) {
        for (j = 0; j < N_BIT_PWM; j++) {
            outListPtr->bcmBuffer[j] = 0xFF;    //Set all bits by default
        }
        outListPtr->i2cChannel = -1;            //This marks the entry as invalid
        outListPtr++;
    }
    xLastWakeTime = xTaskGetTickCount();
    while (1) {
        //---------------------------------
        // This loop does binary code modulation
        // It is executed with increasing delay
        //        at t=1, t=2, t=4, ...
        //---------------------------------
        outListPtr = g_outWriterList;
        for (i = 0; i < OUT_WRITER_LIST_LEN; i++) {
            if (outListPtr->i2cChannel < 0) {
                //This and all further entries in the array are invalid items.
                break;
            } else {
                //A valid item, simply output the current bcm buffer value over I2C
                ts_i2cTransfer(outListPtr->i2cChannel, outListPtr->i2cAddress,
                        &outListPtr->bcmBuffer[bcmCycleCounter], 1, NULL, 0,
                        NULL, NULL);
                handleBitRules(outListPtr, lastTickCount);
            }
            outListPtr++;
        }
        //---------------------------------
        // Measure ticks
        //---------------------------------
        ticks = stopTimer();
        c++;
        if (c >= 3000) {
            c = 0;
            UARTprintf("%22s: %d ticks\n", "taskPCLOutWriter()", ticks);
        }
        //-----------------------------------------
        // Delay until next bit needs to be output
        //-----------------------------------------
        // SET0, 1 ms, SET1, 2 ms, SET2, 4 ms, SET3, 8 ms, repeat
        lastTickCount = (1 << bcmCycleCounter);
        vTaskDelayUntil(&xLastWakeTime, lastTickCount);
        startTimer();
        bcmCycleCounter++;
        if (bcmCycleCounter >= N_BIT_PWM) {
            bcmCycleCounter = 0;
        }
    }
}

void setPclOutput(t_outputBit outLocation, int16_t tPulse, uint8_t highPower,
        uint8_t lowPower) {
//    tPulse    = duration of the pulse [ms]
//    highPower = PWM value during the pulse
//    lowPower  = PWM value after  the pulse
    uint8_t i;
    t_PCLOutputByte *outListPtr = g_outWriterList;
    t_BitModifyRules *bitRules;
    if (outLocation.hwIndexType != HW_INDEX_I2C)
        return;
    for (i = 0; i < OUT_WRITER_LIST_LEN; i++) {
        if (outListPtr->i2cChannel == -1) {
//            Create new entry!
            bitRules = &outListPtr->bitRules[outLocation.pinIndex];
            bitRules->tPulse = tPulse;
            bitRules->lowPWM = lowPower;
            outListPtr->i2cAddress = outLocation.i2cAddress;
            setBcm(outListPtr->bcmBuffer, outLocation.pinIndex, highPower);
            outListPtr->i2cChannel = outLocation.i2cChannel;//Mark the item as valid to the output routine
            return;
        } else if (outListPtr->i2cChannel == outLocation.i2cChannel
                && outListPtr->i2cAddress == outLocation.i2cAddress) {
//            Found the right byte, change it
            bitRules = &outListPtr->bitRules[outLocation.pinIndex];
            bitRules->lowPWM = lowPower;
            setBcm(outListPtr->bcmBuffer, outLocation.pinIndex, highPower);
            bitRules->tPulse = tPulse;
            return;
        }
        outListPtr++;
    }
//    Error, no more space in outList :(
    UARTprintf("%22s: Error, no more space in g_outWriterList :(\n",
            "setPclOutput()");
}

void ts_usbSend(uint8_t *data, uint16_t len) {
//    Do a thread safe USB TX transfer in background (add data to USB send buffer)
    uint32_t freeSpace;
    taskENTER_CRITICAL();
    freeSpace = USBBufferSpaceAvailable(&g_sTxBuffer);
//        UARTprintf( "ts_usbSend(): TX buffer %d bytes free.\n", freeSpace );
    if (freeSpace >= len) {
        USBBufferWrite(&g_sTxBuffer, data, len);
    } else {
        UARTprintf("%22s: Not enough space in USB TX buffer! Need %d have %d\n",
                "ts_usbSend()", len, freeSpace);
    }
    taskEXIT_CRITICAL();
}

t_outputBit decodeHwIndex(uint16_t hwIndex) {
//Meaning of byteIndex:  HW_INDEX_SWM: column,  HW_INDEX_I2Cn: right shited I2C address
    int16_t i2cCh;
    t_outputBit tempResult;
    tempResult.byteIndex = hwIndex / 8;
    tempResult.pinIndex = hwIndex % 8;    // Which bit of the byte is addressed
    tempResult.i2cChannel = -1;
    if (tempResult.byteIndex <= 7) {// byteIndex 0 - 7 are Switch Matrix addresses
        tempResult.hwIndexType = HW_INDEX_SWM;
        return tempResult;
    }
    i2cCh = (tempResult.byteIndex - 8) / 8;    // Only i2c channel 0-3 exists
    if (i2cCh <= 3) {
        tempResult.hwIndexType = HW_INDEX_I2C;
        tempResult.i2cChannel = i2cCh;//I2C address, each channel has address 0x40 - 0x47
        tempResult.i2cAddress = (tempResult.byteIndex - 8) % 8 + 0x40;
        return tempResult;
    }
    tempResult.hwIndexType = HW_INDEX_INVALID;
    return tempResult;
}

// This is the table that holds the command names, implementing functions, and brief description.
tCmdLineEntry g_psCmdTable[] =
        { { "?", Cmd_help, ": Display list of commands" }, { "*IDN?", Cmd_IDN,
                ": Display ID and version info" }, { "SW?", Cmd_SW,
                ": Return the state of ALL switches (40 bytes)" },
                { "OUT", Cmd_OUT,
                        ": OUT hwIndex tPulse PWMhigh PWMlow\nOUT   : OUT hwIndex PWMvalue" },
                { "RUL", Cmd_RUL,
                        ": RUL ID IDin IDout trHoldOff tPulse pwmOn pwmOff\n        bPosEdge bAutoOff bLevelTr" },
                { "RULE", Cmd_RULE,
                        ": Enable  a previously disabled rule: RULE ID" }, {
                        "RULD", Cmd_RULD,
                        ": Disable a previously defined rule:  RULD ID" }, {
                        "LED", Cmd_LED, ": LED <CH>,<led0>,<led1> ..." }, { 0,
                        0, 0 } };

// This function implements the "help" command.  It prints a simple list of the available commands with a brief description.
int Cmd_help(int argc, char *argv[]) {
    tCmdLineEntry *pEntry;
    UARTprintf("\nAvailable commands\n");
    UARTprintf("------------------\n");
    pEntry = &g_psCmdTable[0];    // Point at the beginning of the command table.
    // Enter a loop to read each entry from the command table.  The end of the
    // table has been reached when the command name is NULL.
    while (pEntry->pcCmd) {
        UARTprintf("%6s%s\n", pEntry->pcCmd, pEntry->pcHelp);// Print the command name and the brief description.
        pEntry++;                    // Advance to the next entry in the table.
    }
    return (0);                                                // Return success.
}

int Cmd_IDN(int argc, char *argv[]) {
    UARTprintf( VERSION_INFO);
    return (0);
}

int Cmd_SW(int argc, char *argv[]) {
    // Report state of all switches
    static char outBuffer[REPORT_SWITCH_BUF_SIZE];
    uint16_t charsWritten = 3;
    uint8_t i;
    ustrncpy(outBuffer, "SW:", REPORT_SWITCH_BUF_SIZE); //SW = Hex coded switch state
    for (i = 0; i < N_LONGS; i++) {
        charsWritten += usnprintf(&outBuffer[charsWritten],
                REPORT_SWITCH_BUF_SIZE - charsWritten, "0x%08x,",
                g_SwitchStateDebounced.longValues[i]);
        if (charsWritten >= REPORT_SWITCH_BUF_SIZE - 10) {
            UARTprintf("Cmd_SW(): string buffer overflow!\n");
            return (0);
        }
    }
    outBuffer[charsWritten - 1] = '\n';
    outBuffer[charsWritten] = '\r';
    ts_usbSend((uint8_t*) outBuffer, charsWritten + 1);
    return (0);
}

int Cmd_OUT(int argc, char *argv[]) {
//    OUT <hwIndex> <tPulse> <PWMhigh> <PWMlow>  or OUT <hwIndex> <PWMvalue>
//    OUT 0x0FE 1500 15 1
//    OUT 0x0FE 2
    int32_t hwIndex;
    t_outputBit outLocation;
    uint16_t tPulse, pwmHigh, pwmLow;
    if (argc == 3 || argc == 5) {
        hwIndex = ustrtoul(argv[1], NULL, 0);
        if (argc == 5) {
            tPulse = ustrtoul(argv[2], NULL, 0);
            pwmHigh = ustrtoul(argv[3], NULL, 0);
            pwmLow = ustrtoul(argv[4], NULL, 0);
        } else if (argc == 3) {
            tPulse = 0;
            pwmHigh = ustrtoul(argv[2], NULL, 0);
            pwmLow = pwmHigh;
        }
        if (pwmHigh >= (1 << N_BIT_PWM) || pwmLow >= (1 << N_BIT_PWM)) {
            UARTprintf("Cmd_OUT(): PWMvalue must be < %d\n", (1 << N_BIT_PWM));
            return 0;
        }
        outLocation = decodeHwIndex(hwIndex);
        if (outLocation.hwIndexType == HW_INDEX_I2C) {
            UARTprintf(
                    "Cmd_OUT(): i2cCh %d, i2cAdr 0x%02x, bit %d = tp %d, pH %d, pL %d\n",
                    outLocation.i2cChannel, outLocation.i2cAddress,
                    outLocation.pinIndex, tPulse, pwmHigh, pwmLow);
            setPclOutput(outLocation, tPulse, pwmHigh, pwmLow);
            return (0);
        } else if (outLocation.hwIndexType == HW_INDEX_SWM) {
            UARTprintf("Cmd_OUT(): hwIndex=%d is a SM input\n", hwIndex);
            return (0);
        } else if (outLocation.hwIndexType == HW_INDEX_INVALID) {
            UARTprintf("Cmd_OUT(): HW_INDEX_INVALID: %s\n", argv[1]);
            return (0);
        }
    }
    return ( CMDLINE_TOO_FEW_ARGS);
}

int Cmd_RULE(int argc, char *argv[]) {
    //Enable a quickfire rule
    uint8_t id;
    if (argc == 2) {
        id = ustrtoul(argv[1], NULL, 0);
        if (id >= MAX_QUICK_RULES) {
            UARTprintf("%22s: quickRuleId must be < %d\n", "Cmd_RULE()",
                    MAX_QUICK_RULES);
            return 0;
        }
        enableQuickRule(id);
        return (0);
    }
    return CMDLINE_TOO_FEW_ARGS;
}

int Cmd_RULD(int argc, char *argv[]) {
    //Disable a quickfire rule
    uint8_t id;
    if (argc == 2) {
        id = ustrtoul(argv[1], NULL, 0);
        if (id >= MAX_QUICK_RULES) {
            UARTprintf("%22s: quickRuleId must be < %d\n", "Cmd_RULD()",
                    MAX_QUICK_RULES);
            return 0;
        }
        disableQuickRule(id);
        return (0);
    }
    return CMDLINE_TOO_FEW_ARGS;
}

int Cmd_RUL(int argc, char *argv[]) {
// Configure and activate QuickRule:
//  * quickRuleId (0-64)
//  * input switch ID number
//  * driver output ID number
//  * post trigger hold-off time [ms]
//  * pulse duration [ms]
//  * pulse pwm [only for output ID 0-3 which are the pwm channels]
//  * hold pwm  [only for output ID 0-3 which are the pwm channels]
//  * Enable trigger on pos edge?
//  * Enable auto. output off once input releases
//  * Enable level Trigger (no edge check)
//  ------------------
//   Example command:
//  ------------------
//  RUL ID IDin IDout trHoldOff tPulse pwmOn pwmOff bPosEdge bAutoOff bLevelTr
//  RUL 0 0x23 0x100 4 1 15 3 1 0 0
    uint8_t id, pwmHigh, pwmLow;
    uint16_t triggerHoldOffTime, tPulse;
    t_outputBit inputSwitchId, outputDriverId;
    bool trigPosEdge, outOffOnRelease, levelTriggered;
    if (argc == 11) {
        id = ustrtoul(argv[1], NULL, 0);
        if (id >= MAX_QUICK_RULES) {
            UARTprintf("%22s: quickRuleId must be < %d\n", "Cmd_RUL()",
                    MAX_QUICK_RULES);
            return 0;
        }
        inputSwitchId = decodeHwIndex(ustrtoul(argv[2], NULL, 0));
        if (inputSwitchId.hwIndexType == HW_INDEX_INVALID) {
            UARTprintf("%22s: inputSwitchId = %s invalid\n", "Cmd_RUL()",
                    argv[2]);
            return 0;
        }
        outputDriverId = decodeHwIndex(ustrtoul(argv[3], NULL, 0));
        if (outputDriverId.hwIndexType == HW_INDEX_INVALID
                || outputDriverId.hwIndexType == HW_INDEX_SWM) {
            UARTprintf("%22s: outputDriverId = %s invalid\n", "Cmd_RUL()",
                    argv[3]);
            return 0;
        }
        triggerHoldOffTime = ustrtoul(argv[4], NULL, 0);
        tPulse = ustrtoul(argv[5], NULL, 0);
        pwmHigh = ustrtoul(argv[6], NULL, 0);
        pwmLow = ustrtoul(argv[7], NULL, 0);
        if (pwmHigh >= (1 << N_BIT_PWM) || pwmLow >= (1 << N_BIT_PWM)) {
            UARTprintf("%22s: pwmValues must be < %d\n", "Cmd_RUL()",
                    (1 << N_BIT_PWM));
            return 0;
        }
        trigPosEdge = ustrtoul(argv[8], NULL, 0) == 1;
        outOffOnRelease = ustrtoul(argv[9], NULL, 0) == 1;
        levelTriggered = ustrtoul(argv[10], NULL, 0) == 1;
        UARTprintf("%22s: Setting up autofiring rule %d\n", "Cmd_RUL()", id);
        setupQuickRule(id, inputSwitchId, outputDriverId, triggerHoldOffTime,
                tPulse, pwmHigh, pwmLow, trigPosEdge, outOffOnRelease,
                levelTriggered);
        return (0);
    }
    return CMDLINE_TOO_FEW_ARGS;
}

int Cmd_LED(int argc, char *argv[]) {
    UARTprintf("Blasting data to LED string on channel n\n");
    return (0);
}

