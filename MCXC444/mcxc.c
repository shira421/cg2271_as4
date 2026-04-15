/*
 * =============================================================================
 * PetPal v2 - MCXC444 (FreeRTOS Integration)
 * =============================================================================
 * Ultrasonic + Servo + LED + UART2 to ESP32
 *
 * Sends distance to ESP32, receives:
 *   - Pet detection status (LED control)
 *   - Feed command from dashboard (servo opens gate)
 *   - Play command from dashboard (servo sweeps + laser on ESP32)
 *   - Stop command from dashboard (servo centers)
 *
 * Pins:
 *   HC-SR04 Trig  : PTD2  (GPIO output)
 *   HC-SR04 Echo  : PTD4  (GPIO input, interrupt both edges)
 *   Food servo    : PTC1  (TPM0_CH0, PWM 50Hz)
 *   Laser servo   : PTC2  (TPM0_CH1, PWM 50Hz)
 *   Onboard LED   : PTD5  (active-low green LED)
 *   UART2 TX      : PTE22 (Alt4) -> ESP32 GPIO 18 (RX)
 *   UART2 RX      : PTE23 (Alt4) -> ESP32 GPIO 17 (TX)
 *
 * Protocol:
 *   MCX -> ESP:  [0xAA][type][data...]
 *     0x01 = distance:  [0xAA][0x01][dist_hi][dist_lo]
 *
 *   ESP -> MCX:  [0xBB][type][data...]
 *     0x01 = pet status: [0xBB][0x01][0x00 or 0x01]
 *     0x10 = feed now:   [0xBB][0x10]
 *     0x11 = play start: [0xBB][0x11]
 *     0x12 = stop:       [0xBB][0x12]
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

/* Food dispenser servo on PTC1 -> TPM0_CH0 */
#define FOOD_SERVO_PIN      1U
#define FOOD_SERVO_CHANNEL  0U

/* Laser sweep servo on PTC2 -> TPM0_CH1 */
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

#define TIMER_CLK       375000
#define SERVO_MOD       ((TIMER_CLK / 50) - 1)

/* Feed gate timing */
#define FEED_DURATION_MS 3000   /* Keep gate open for 3 seconds */

/* ========================= COMMAND TYPES FROM ESP32 ====================== */
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

/* Ultrasonic */
static volatile uint16_t echoStartTick = 0;
static volatile uint16_t lastDistance = 999;

/* ========================= SIMPLE DELAYS ================================= */

static void delay_us(uint32_t us)
{
    volatile uint32_t c = us * 12;
    while (c--) { __NOP(); }
}

/* ========================= ISR: ULTRASONIC ECHO ========================== */

void PORTC_PORTD_IRQHandler(void)
{
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    uint32_t flags = GPIO_PortGetInterruptFlags(ECHO_GPIO);

    if (flags & (1U << ECHO_PIN)) {
        if (GPIO_PinRead(ECHO_GPIO, ECHO_PIN)) {
            echoStartTick = STOPWATCH_TPM->CNT;
        } else {
            uint16_t endTick = STOPWATCH_TPM->CNT;
            uint16_t elapsed = endTick - echoStartTick;
            uint32_t pulse_us = elapsed * 16U;

            if (pulse_us > 50 && pulse_us < 25000) {
                lastDistance = (uint16_t)(pulse_us / 58);
            } else {
                lastDistance = 999;
            }

            if (echoSemaphore != NULL) {
                xSemaphoreGiveFromISR(echoSemaphore, &higherPriorityTaskWoken);
            }
        }
        GPIO_PortClearInterruptFlags(ECHO_GPIO, 1U << ECHO_PIN);
    }

    portYIELD_FROM_ISR(higherPriorityTaskWoken);
}

/* ========================= ISR: UART2 RX ================================= */

void UART2_FLEXIO_IRQHandler(void)
{
    BaseType_t higherPriorityTaskWoken = pdFALSE;

    if (UART_GetStatusFlags(ESP_UART) & kUART_RxDataRegFullFlag) {
        uint8_t data = UART_ReadByte(ESP_UART);
        if (cmdQueue != NULL) {
            xQueueSendFromISR(cmdQueue, &data, &higherPriorityTaskWoken);
        }
    }

    if (UART_GetStatusFlags(ESP_UART) & kUART_RxOverrunFlag) {
        (void)ESP_UART->S1;
        (void)ESP_UART->D;
    }

    portYIELD_FROM_ISR(higherPriorityTaskWoken);
}

/* ========================= UART TX ======================================= */

static void uart_send_byte(uint8_t b)
{
    while (!(UART_GetStatusFlags(ESP_UART) & kUART_TxDataRegEmptyFlag)) {}
    UART_WriteByte(ESP_UART, b);
}

static void send_distance(uint16_t dist_cm)
{
    uart_send_byte(0xAA);
    uart_send_byte(0x01);
    uart_send_byte((dist_cm >> 8) & 0xFF);
    uart_send_byte(dist_cm & 0xFF);
}

/* ========================= SERVO ========================================= */

static void food_servo_set(uint16_t pulse_us) {
    if (pulse_us < 500) pulse_us = 500;
    if (pulse_us > 2500) pulse_us = 2500;
    SERVO_TPM->CONTROLS[FOOD_SERVO_CHANNEL].CnV = (pulse_us * 375) / 1000;
}

static void laser_servo_set(uint16_t pulse_us) {
    if (pulse_us < 500) pulse_us = 500;
    if (pulse_us > 2500) pulse_us = 2500;
    SERVO_TPM->CONTROLS[LASER_SERVO_CHANNEL].CnV = (pulse_us * 375) / 1000;
}

/* ========================= RTOS TASKS ==================================== */

static void sensor_task(void *pvParameters)
{
    (void)pvParameters;

    while (1) {
        xSemaphoreTake(echoSemaphore, 0);

        GPIO_PinWrite(TRIG_GPIO, TRIG_PIN, 1U);
        delay_us(10);
        GPIO_PinWrite(TRIG_GPIO, TRIG_PIN, 0U);

        if (xSemaphoreTake(echoSemaphore, pdMS_TO_TICKS(60)) == pdTRUE) {
            send_distance(lastDistance);
        } else {
            send_distance(999);
        }

        vTaskDelay(pdMS_TO_TICKS(140));
    }
}

static void command_task(void *pvParameters)
{
    (void)pvParameters;

    uint8_t b;
    uint8_t state = 0;
    uint8_t rxType = 0;

    while (1) {
        if (xQueueReceive(cmdQueue, &b, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (state) {
        case 0:
            if (b == 0xBB) {
                state = 1;
            }
            break;

        case 1:
            rxType = b;
            if (rxType == CMD_PET_STATUS) {
                state = 2;
                break;
            }

            if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
                if (rxType == CMD_FEED) {
                    PRINTF("[CMD] Feed from dashboard\r\n");
                    currentMode = MODE_FEEDING;
                } else if (rxType == CMD_PLAY) {
                    PRINTF("[CMD] Play from dashboard\r\n");
                    currentMode = MODE_PLAYING;
                } else if (rxType == CMD_STOP) {
                    PRINTF("[CMD] Stop from dashboard\r\n");
                    currentMode = MODE_IDLE;
                }
                xSemaphoreGive(stateMutex);
            }
            state = 0;
            break;

        case 2:
            if (rxType == CMD_PET_STATUS) {
                if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
                    petDetected = b;
                    xSemaphoreGive(stateMutex);
                }
            }
            state = 0;
            break;

        default:
            state = 0;
            break;
        }
    }
}

static void read_state(SystemMode_t *mode, uint8_t *pet)
{
    if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
        *mode = currentMode;
        *pet = petDetected;
        xSemaphoreGive(stateMutex);
    }
}

static void reset_mode_if_feeding(void)
{
    if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
        if (currentMode == MODE_FEEDING) {
            currentMode = MODE_IDLE;
        }
        xSemaphoreGive(stateMutex);
    }
}

static BaseType_t feeding_still_active(void)
{
    BaseType_t active = pdFALSE;

    if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
        active = (currentMode == MODE_FEEDING) ? pdTRUE : pdFALSE;
        xSemaphoreGive(stateMutex);
    }

    return active;
}

static void actuator_task(void *pvParameters)
{
    (void)pvParameters;

    uint16_t sweepPos = SERVO_CENTER;
    int16_t sweepDir = 20;
    SystemMode_t localMode = MODE_IDLE;
    uint8_t localPet = 0;
    uint8_t i;

    while (1) {
        read_state(&localMode, &localPet);

        if (localPet) {
            LED_OFF();
        } else {
            LED_ON();
        }

        switch (localMode) {
        case MODE_FEEDING:
            laser_servo_set(SERVO_CENTER);
            food_servo_set(SERVO_FEED_OPEN);

            for (i = 0; i < 30 && feeding_still_active(); i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }

            if (feeding_still_active()) {
                food_servo_set(SERVO_FEED_CLOSED);
                vTaskDelay(pdMS_TO_TICKS(500));
                food_servo_set(SERVO_CENTER);
                reset_mode_if_feeding();
                PRINTF("[FEED] Done, gate closed\r\n");
            } else {
                food_servo_set(SERVO_CENTER);
            }
            break;

        case MODE_PLAYING:
            food_servo_set(SERVO_CENTER);
            sweepPos += sweepDir;
            if (sweepPos >= SERVO_RIGHT) {
                sweepPos = SERVO_RIGHT;
                sweepDir = -sweepDir;
            }
            if (sweepPos <= SERVO_LEFT) {
                sweepPos = SERVO_LEFT;
                sweepDir = -sweepDir;
            }
            laser_servo_set(sweepPos);
            vTaskDelay(pdMS_TO_TICKS(50));
            break;

        case MODE_IDLE:
        default:
            food_servo_set(SERVO_CENTER);
            laser_servo_set(SERVO_CENTER);
            sweepPos = SERVO_CENTER;
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        }
    }
}
/* ========================= MAIN ========================================== */

int main(void)
{
    gpio_pin_config_t outCfg = { .pinDirection = kGPIO_DigitalOutput, .outputLogic = 0 };
    gpio_pin_config_t inCfg  = { .pinDirection = kGPIO_DigitalInput,  .outputLogic = 0 };
    tpm_config_t tpmCfg;
    uart_config_t uartCfg;

    BOARD_InitBootPins();
    BOARD_InitBootClocks();
    BOARD_InitBootPeripherals();
#ifndef BOARD_INIT_DEBUG_CONSOLE_PERIPHERAL
    BOARD_InitDebugConsole();
#endif

    PRINTF("\r\n========================================\r\n");
    PRINTF("  PetPal v2 - MCXC444 FreeRTOS\r\n");
    PRINTF("  Ultrasonic + Servo + LED + UART2\r\n");
    PRINTF("========================================\r\n\r\n");

    /* Enable clocks */
    CLOCK_EnableClock(kCLOCK_PortA);
    CLOCK_EnableClock(kCLOCK_PortB);
    CLOCK_EnableClock(kCLOCK_PortC);
    CLOCK_EnableClock(kCLOCK_PortD);
    CLOCK_EnableClock(kCLOCK_PortE);
    CLOCK_EnableClock(kCLOCK_Uart2);

    /* Ultrasonic */
    PORT_SetPinMux(TRIG_PORT, TRIG_PIN, kPORT_MuxAsGpio);
    GPIO_PinInit(TRIG_GPIO, TRIG_PIN, &outCfg);
    PORT_SetPinMux(ECHO_PORT, ECHO_PIN, kPORT_MuxAsGpio);
    GPIO_PinInit(ECHO_GPIO, ECHO_PIN, &inCfg);
    PORT_SetPinInterruptConfig(ECHO_PORT, ECHO_PIN, kPORT_InterruptEitherEdge);
    NVIC_SetPriority(PORTC_PORTD_IRQn, 5);
    PRINTF("[INIT] Ultrasonic: Trig=PTD%d, Echo=PTD%d\r\n", TRIG_PIN, ECHO_PIN);

    /* LED */
    PORT_SetPinMux(LED_PORT, LED_PIN, kPORT_MuxAsGpio);
    gpio_pin_config_t ledCfg = { .pinDirection = kGPIO_DigitalOutput, .outputLogic = 0 };
    GPIO_PinInit(LED_GPIO, LED_PIN, &ledCfg);
    LED_ON();
    PRINTF("[INIT] LED: PTD%d (ON)\r\n", LED_PIN);

    /* Servo */
    /* --- Servo PWM (Port C) --- */
    CLOCK_SetTpmClock(1U);

        /* MUST set pin mux AFTER BOARD_InitBootPins to override any conflicts */
	PORT_SetPinMux(SERVO_PORT, FOOD_SERVO_PIN,  kPORT_MuxAlt4);  /* PTC1 -> TPM0_CH0 */
	PORT_SetPinMux(SERVO_PORT, LASER_SERVO_PIN, kPORT_MuxAlt4);  /* PTC2 -> TPM0_CH1 */

	TPM_GetDefaultConfig(&tpmCfg);
	tpmCfg.prescale = kTPM_Prescale_Divide_128;
	TPM_Init(SERVO_TPM, &tpmCfg);

	/* Food servo channel */
	SERVO_TPM->CONTROLS[FOOD_SERVO_CHANNEL].CnSC = TPM_CnSC_MSB_MASK | TPM_CnSC_ELSB_MASK;
	SERVO_TPM->CONTROLS[FOOD_SERVO_CHANNEL].CnV = SERVO_CENTER / 16;

	/* Laser servo channel */
	SERVO_TPM->CONTROLS[LASER_SERVO_CHANNEL].CnSC = TPM_CnSC_MSB_MASK | TPM_CnSC_ELSB_MASK;
	SERVO_TPM->CONTROLS[LASER_SERVO_CHANNEL].CnV = SERVO_CENTER / 16;

	SERVO_TPM->MOD = SERVO_MOD;
	TPM_StartTimer(SERVO_TPM, kTPM_SystemClock);
	PRINTF("[INIT] Food servo: PTC%d (CH%d)\r\n", FOOD_SERVO_PIN, FOOD_SERVO_CHANNEL);
	PRINTF("[INIT] Laser servo: PTC%d (CH%d)\r\n", LASER_SERVO_PIN, LASER_SERVO_CHANNEL);

    /* Stopwatch timer for ultrasonic echo pulse width. With the TPM clock used
     * above and /128 prescale, each tick is about 16 us.
     */
    TPM_Init(STOPWATCH_TPM, &tpmCfg);
    STOPWATCH_TPM->MOD = 0xFFFF;
    TPM_StartTimer(STOPWATCH_TPM, kTPM_SystemClock);

    /* UART2 */
    PORT_SetPinMux(ESP_TX_PORT, ESP_TX_PIN, kPORT_MuxAlt4);
    PORT_SetPinMux(ESP_RX_PORT, ESP_RX_PIN, kPORT_MuxAlt4);
    UART_GetDefaultConfig(&uartCfg);
    uartCfg.baudRate_Bps = ESP_UART_BAUD;
    uartCfg.enableTx = true;
    uartCfg.enableRx = true;
    UART_Init(ESP_UART, &uartCfg, CLOCK_GetBusClkFreq());
    UART_EnableInterrupts(ESP_UART, kUART_RxDataRegFullInterruptEnable |
                                     kUART_RxOverrunInterruptEnable);
    NVIC_SetPriority(UART2_FLEXIO_IRQn, 5);
    PRINTF("[INIT] UART2: TX=PTE%d, RX=PTE%d\r\n\r\n", ESP_TX_PIN, ESP_RX_PIN);

    echoSemaphore = xSemaphoreCreateBinary();
    stateMutex = xSemaphoreCreateMutex();
    cmdQueue = xQueueCreate(32, sizeof(uint8_t));

    if (echoSemaphore == NULL || stateMutex == NULL || cmdQueue == NULL) {
        PRINTF("[ERR] Failed to create RTOS primitives\r\n");
        while (1) {}
    }

    EnableIRQ(PORTC_PORTD_IRQn);
    EnableIRQ(UART2_FLEXIO_IRQn);

    if (xTaskCreate(sensor_task, "Sensor", configMINIMAL_STACK_SIZE + 128, NULL, 2, NULL) != pdPASS ||
        xTaskCreate(command_task, "Command", configMINIMAL_STACK_SIZE + 256, NULL, 3, NULL) != pdPASS ||
        xTaskCreate(actuator_task, "Actuator", configMINIMAL_STACK_SIZE + 256, NULL, 2, NULL) != pdPASS) {
        PRINTF("[ERR] Failed to create RTOS tasks\r\n");
        while (1) {}
    }

    PRINTF("[RUN] LED ON = no pet. LED OFF = pet detected.\r\n");
    PRINTF("[RUN] FreeRTOS scheduler starting.\r\n\r\n");
    vTaskStartScheduler();

    while (1) {}

    return 0;
}
