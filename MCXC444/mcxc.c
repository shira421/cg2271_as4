/*
 * =============================================================================
 * PetPal v2 - MCXC444 Final FreeRTOS Integration
 * =============================================================================
 * ESP32 UART Bridge Version
 * * Pins:
 * HC-SR04 Trig  : PTD2  (GPIO output)
 * HC-SR04 Echo  : PTD4  (GPIO input, interrupt both edges)
 * Food servo    : PTC1  (TPM0_CH0, PWM 50Hz)
 * Laser servo   : PTC2  (TPM0_CH1, PWM 50Hz)
 * Onboard LED   : PTD5  (active-low green LED)
 * UART2 TX      : PTE22 (Alt4) -> ESP32 RX
 * UART2 RX      : PTE23 (Alt4) -> ESP32 TX
 *
 * Protocol:
 * MCX -> ESP:  [0xAA][0x01][dist_hi][dist_lo]
 * ESP -> MCX:  [0xBB][type][data] (0x01=Pet, 0x10=Feed, 0x11=Play, 0x12=Stop)
 * =============================================================================
 */

#include "board.h"
#include "peripherals.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "fsl_gpio.h"
#include "fsl_port.h"
#include "fsl_tpm.h"
#include "fsl_uart.h"
#include "fsl_debug_console.h"
#include "fsl_common.h"

/* FreeRTOS */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/* ========================= PIN DEFINITIONS =============================== */
#define TRIG_PORT       PORTD
#define TRIG_GPIO       GPIOD
#define TRIG_PIN        2U

#define ECHO_PORT       PORTD
#define ECHO_GPIO       GPIOD
#define ECHO_PIN        4U

#define SERVO_TPM       TPM0
#define SERVO_PORT      PORTC
#define STOPWATCH_TPM   TPM1

#define FOOD_SERVO_PIN      1U
#define FOOD_SERVO_CHANNEL  0U
#define LASER_SERVO_PIN     2U
#define LASER_SERVO_CHANNEL 1U

#define LED_PORT        PORTD
#define LED_GPIO        GPIOD
#define LED_PIN         5U
#define LED_ON()        GPIO_PinWrite(LED_GPIO, LED_PIN, 0U)
#define LED_OFF()       GPIO_PinWrite(LED_GPIO, LED_PIN, 1U)

#define ESP_UART        UART2
#define ESP_UART_BAUD   115200U
#define ESP_TX_PORT     PORTE
#define ESP_TX_PIN      22U
#define ESP_RX_PORT     PORTE
#define ESP_RX_PIN      23U

/* Servo positions */
#define SERVO_CENTER    1500
#define SERVO_LEFT      800
#define SERVO_RIGHT     2200
#define SERVO_FEED_OPEN 2000
#define SERVO_FEED_CLOSED 1000

/* Correct Math for 48MHz Clock (375kHz Timer) */
#define TIMER_CLK       375000
#define SERVO_MOD       ((TIMER_CLK / 50) - 1)

#define CMD_PET_STATUS  0x01
#define CMD_FEED        0x10
#define CMD_PLAY        0x11
#define CMD_STOP        0x12

/* ========================= SYSTEM STATE ================================== */
typedef enum {
    MODE_IDLE,
    MODE_FEEDING,
    MODE_PLAYING
} SystemMode_t;

static SystemMode_t currentMode = MODE_IDLE;
static uint8_t petDetected = 0;

static QueueHandle_t cmdQueue = NULL;
static SemaphoreHandle_t echoSemaphore = NULL;
static SemaphoreHandle_t stateMutex = NULL;

static volatile uint16_t echoStartTick = 0;
static volatile uint16_t lastDistance = 999;

/* ========================= HELPERS & ISRs ================================ */
static void delay_us(uint32_t us) {
    volatile uint32_t c = us * 12;
    while (c--) { __NOP(); }
}

void PORTC_PORTD_IRQHandler(void) {
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    uint32_t flags = GPIO_PortGetInterruptFlags(ECHO_GPIO);

    if (flags & (1U << ECHO_PIN)) {
        if (GPIO_PinRead(ECHO_GPIO, ECHO_PIN)) {
            echoStartTick = STOPWATCH_TPM->CNT;
        } else {
            uint16_t endTick = STOPWATCH_TPM->CNT;
            uint16_t elapsed = endTick - echoStartTick;

            /* TPM1 ticks at 375kHz. 1 tick = ~2.66us */
            uint32_t pulse_us = (elapsed * 8) / 3;

            if (pulse_us > 100 && pulse_us < 25000) lastDistance = (uint16_t)(pulse_us / 58);
            else lastDistance = 999;

            if (echoSemaphore != NULL) xSemaphoreGiveFromISR(echoSemaphore, &higherPriorityTaskWoken);
        }
        GPIO_PortClearInterruptFlags(ECHO_GPIO, 1U  << ECHO_PIN);
    }
    portYIELD_FROM_ISR(higherPriorityTaskWoken);
}

void UART2_FLEXIO_IRQHandler(void) {
    BaseType_t higherPriorityTaskWoken = pdFALSE;

    if (UART_GetStatusFlags(ESP_UART) & kUART_RxDataRegFullFlag) {
        uint8_t data = UART_ReadByte(ESP_UART);
        if (cmdQueue != NULL) xQueueSendFromISR(cmdQueue, &data, &higherPriorityTaskWoken);
    }
    if (UART_GetStatusFlags(ESP_UART) & kUART_RxOverrunFlag) {
        (void)ESP_UART->S1;
        (void)ESP_UART->D;
    }
    portYIELD_FROM_ISR(higherPriorityTaskWoken);
}

static void uart_send_byte(uint8_t b) {
    while (!(UART_GetStatusFlags(ESP_UART) & kUART_TxDataRegEmptyFlag)) {}
    UART_WriteByte(ESP_UART, b);
}

static void send_distance(uint16_t dist_cm) {
    uint8_t hi = (dist_cm >> 8) & 0xFF;
    uint8_t lo = dist_cm & 0xFF;
    uint8_t chk = 0x01 ^ hi ^ lo;
    uart_send_byte(0xAA); uart_send_byte(0x01);
    uart_send_byte(hi); uart_send_byte(lo); uart_send_byte(chk);
}

static void food_servo_set(uint16_t pulse_us) {
    if (pulse_us < 500) pulse_us = 500; if (pulse_us > 2500) pulse_us = 2500;
    SERVO_TPM->CONTROLS[FOOD_SERVO_CHANNEL].CnV = (pulse_us * 375) / 1000;
}

static void laser_servo_set(uint16_t pulse_us) {
    if (pulse_us < 500) pulse_us = 500; if (pulse_us > 2500) pulse_us = 2500;
    SERVO_TPM->CONTROLS[LASER_SERVO_CHANNEL].CnV = (pulse_us * 375) / 1000;
}

/* ========================= RTOS TASKS ==================================== */

/* Task 1: Trigger ultrasonic and send distance over UART */
static void sensor_task(void *pvParameters) {
    while (1) {
        xSemaphoreTake(echoSemaphore, 0);
        GPIO_PinWrite(TRIG_GPIO, TRIG_PIN, 1U); delay_us(10); GPIO_PinWrite(TRIG_GPIO, TRIG_PIN, 0U);

        if (xSemaphoreTake(echoSemaphore, pdMS_TO_TICKS(60)) == pdTRUE) send_distance(lastDistance);
        else send_distance(999);

        vTaskDelay(pdMS_TO_TICKS(140));
    }
}

/* Task 2: Listen to ESP32 commands */
static void command_task(void *pvParameters) {
    uint8_t b, state = 0, rxType = 0, rxData = 0;
    while (1) {
        if (xQueueReceive(cmdQueue, &b, portMAX_DELAY) != pdTRUE) continue;

        switch (state) {
        case 0:
            if (b == 0xBB) state = 1;
            break;

        case 1:
            rxType = b;
            if (rxType == CMD_PET_STATUS) {
                state = 2; /* next byte is data */
            } else {
                state = 4; /* next byte is checksum (no data) */
            }
            break;

        case 2:
            /* Data byte for CMD_PET_STATUS */
            rxData = b;
            state = 3; /* next byte is checksum */
            break;

        case 3:
        {
            /* Checksum for commands with data: type ^ data */
            uint8_t expected = rxType ^ rxData;
            if (b == expected) {
                if (rxType == CMD_PET_STATUS) {
                    if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
                        petDetected = rxData;
                        xSemaphoreGive(stateMutex);
                    }
                }
            } else {
                PRINTF("[UART] Checksum fail: got 0x%02X expected 0x%02X\r\n", b, expected);
            }
            state = 0;
            break;
        }

        case 4:
        {
            /* Checksum for commands without data: type */
            uint8_t expected = rxType;
            if (b == expected) {
                if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
                    if (rxType == CMD_FEED)      { PRINTF("[CMD] Feed\r\n"); currentMode = MODE_FEEDING; }
                    else if (rxType == CMD_PLAY)  { PRINTF("[CMD] Play\r\n"); currentMode = MODE_PLAYING; }
                    else if (rxType == CMD_STOP)  { PRINTF("[CMD] Stop\r\n"); currentMode = MODE_IDLE; }
                    xSemaphoreGive(stateMutex);
                }
            } else {
                PRINTF("[UART] Checksum fail: got 0x%02X expected 0x%02X\r\n", b, expected);
            }
            state = 0;
            break;
        }

        default:
            state = 0;
            break;
        }
    }
}

static BaseType_t feeding_still_active(void) {
    BaseType_t active = pdFALSE;
    if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
        active = (currentMode == MODE_FEEDING) ? pdTRUE : pdFALSE;
        xSemaphoreGive(stateMutex);
    }
    return active;
}

/* Task 3: Drive Hardware */
static void actuator_task(void *pvParameters) {
    uint16_t sweepPos = SERVO_CENTER;
    int16_t sweepDir = 20;
    SystemMode_t localMode = MODE_IDLE;
    uint8_t localPet = 0, i;

    while (1) {
        if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
            localMode = currentMode; localPet = petDetected;
            xSemaphoreGive(stateMutex);
        }

        if (localPet) LED_OFF(); else LED_ON();

        switch (localMode) {
        case MODE_FEEDING:
            /* Stagger movement: Park laser first, wait, then open food gate */
            laser_servo_set(SERVO_CENTER);
            vTaskDelay(pdMS_TO_TICKS(300));
            food_servo_set(SERVO_FEED_OPEN);

            for (i = 0; i < 30 && feeding_still_active(); i++) vTaskDelay(pdMS_TO_TICKS(100));

            if (feeding_still_active()) {
                food_servo_set(SERVO_FEED_CLOSED);
                vTaskDelay(pdMS_TO_TICKS(500));
                food_servo_set(SERVO_CENTER);
                if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
                    if (currentMode == MODE_FEEDING) currentMode = MODE_IDLE;
                    xSemaphoreGive(stateMutex);
                }
                PRINTF("[FEED] Gate Closed\r\n");
            } else food_servo_set(SERVO_CENTER);
            break;

        case MODE_PLAYING:
            food_servo_set(SERVO_CENTER);
            sweepPos += sweepDir;
            if (sweepPos >= SERVO_RIGHT) { sweepPos = SERVO_RIGHT; sweepDir = -sweepDir; }
            if (sweepPos <= SERVO_LEFT)  { sweepPos = SERVO_LEFT;  sweepDir = -sweepDir; }
            laser_servo_set(sweepPos);
            vTaskDelay(pdMS_TO_TICKS(50));
            break;

        case MODE_IDLE:
        default:
            food_servo_set(SERVO_CENTER); laser_servo_set(SERVO_CENTER);
            sweepPos = SERVO_CENTER;
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        }
    }
}

/* ========================= MAIN ========================================== */
int main(void) {
    gpio_pin_config_t outCfg = { .pinDirection = kGPIO_DigitalOutput, .outputLogic = 0 };
    gpio_pin_config_t inCfg  = { .pinDirection = kGPIO_DigitalInput,  .outputLogic = 0 };
    tpm_config_t tpmCfg; uart_config_t uartCfg;

    BOARD_InitBootPins(); BOARD_InitBootClocks(); BOARD_InitBootPeripherals();
#ifndef BOARD_INIT_DEBUG_CONSOLE_PERIPHERAL
    BOARD_InitDebugConsole();
#endif

    PRINTF("\r\n========================================\r\n");
    PRINTF("  PetPal v2 - MCXC444 Final RTOS Integration\r\n");
    PRINTF("========================================\r\n\r\n");

    CLOCK_EnableClock(kCLOCK_PortA); CLOCK_EnableClock(kCLOCK_PortB);
    CLOCK_EnableClock(kCLOCK_PortC); CLOCK_EnableClock(kCLOCK_PortD);
    CLOCK_EnableClock(kCLOCK_PortE); CLOCK_EnableClock(kCLOCK_Uart2);

    /* Ultrasonic & LED */
    PORT_SetPinMux(TRIG_PORT, TRIG_PIN, kPORT_MuxAsGpio); GPIO_PinInit(TRIG_GPIO, TRIG_PIN, &outCfg);
    PORT_SetPinMux(ECHO_PORT, ECHO_PIN, kPORT_MuxAsGpio); GPIO_PinInit(ECHO_GPIO, ECHO_PIN, &inCfg);
    PORT_SetPinInterruptConfig(ECHO_PORT, ECHO_PIN, kPORT_InterruptEitherEdge);
    PORT_SetPinMux(LED_PORT, LED_PIN, kPORT_MuxAsGpio); GPIO_PinInit(LED_GPIO, LED_PIN, &outCfg);
    LED_ON();

    /* Servos (TPM0) */
    CLOCK_SetTpmClock(1U);
    PORT_SetPinMux(SERVO_PORT, FOOD_SERVO_PIN,  kPORT_MuxAlt4);
    PORT_SetPinMux(SERVO_PORT, LASER_SERVO_PIN, kPORT_MuxAlt4);
    TPM_GetDefaultConfig(&tpmCfg); tpmCfg.prescale = kTPM_Prescale_Divide_128;
    TPM_Init(SERVO_TPM, &tpmCfg);
    SERVO_TPM->CONTROLS[FOOD_SERVO_CHANNEL].CnSC = TPM_CnSC_MSB_MASK | TPM_CnSC_ELSB_MASK;
    SERVO_TPM->CONTROLS[FOOD_SERVO_CHANNEL].CnV = (SERVO_CENTER * 375) / 1000;
    SERVO_TPM->CONTROLS[LASER_SERVO_CHANNEL].CnSC = TPM_CnSC_MSB_MASK | TPM_CnSC_ELSB_MASK;
    SERVO_TPM->CONTROLS[LASER_SERVO_CHANNEL].CnV = (SERVO_CENTER * 375) / 1000;
    SERVO_TPM->MOD = SERVO_MOD; TPM_StartTimer(SERVO_TPM, kTPM_SystemClock);

    /* Stopwatch (TPM1) */
    TPM_GetDefaultConfig(&tpmCfg); tpmCfg.prescale = kTPM_Prescale_Divide_128;
    TPM_Init(STOPWATCH_TPM, &tpmCfg); STOPWATCH_TPM->MOD = 0xFFFF;
    TPM_StartTimer(STOPWATCH_TPM, kTPM_SystemClock);

    /* UART2 */
    PORT_SetPinMux(ESP_TX_PORT, ESP_TX_PIN, kPORT_MuxAlt4);
    PORT_SetPinMux(ESP_RX_PORT, ESP_RX_PIN, kPORT_MuxAlt4);
    UART_GetDefaultConfig(&uartCfg); uartCfg.baudRate_Bps = ESP_UART_BAUD;
    uartCfg.enableTx = true; uartCfg.enableRx = true;
    UART_Init(ESP_UART, &uartCfg, CLOCK_GetBusClkFreq());
    UART_EnableInterrupts(ESP_UART, kUART_RxDataRegFullInterruptEnable | kUART_RxOverrunInterruptEnable);

    /* RTOS Setup */
    echoSemaphore = xSemaphoreCreateBinary();
    stateMutex = xSemaphoreCreateMutex();
    cmdQueue = xQueueCreate(32, sizeof(uint8_t));

    NVIC_SetPriority(PORTC_PORTD_IRQn, 5); EnableIRQ(PORTC_PORTD_IRQn);
    NVIC_SetPriority(UART2_FLEXIO_IRQn, 5); EnableIRQ(UART2_FLEXIO_IRQn);

    xTaskCreate(sensor_task, "Sensor", configMINIMAL_STACK_SIZE + 128, NULL, 2, NULL); // 2nd highest
    xTaskCreate(command_task, "Command", configMINIMAL_STACK_SIZE + 256, NULL, 3, NULL); //highest priority
    xTaskCreate(actuator_task, "Actuator", configMINIMAL_STACK_SIZE + 256, NULL, 2, NULL); //2nd highest

    PRINTF("FreeRTOS starting. Waiting for ESP32 Commands...\r\n");
    vTaskStartScheduler();
    while (1) {} return 0;
}
