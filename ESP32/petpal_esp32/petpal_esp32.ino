/*
 * =============================================================================
 * PetPal v3 - ESP32-S2 Unified (MPU6050 Integration)
 * =============================================================================
 * Sensors + Firebase Realtime Database + UART bridge to MCXC444
 *
 * Protocol with MCXC444:
 *   MCX -> ESP:  [0xAA][0x01][dist_hi][dist_lo]    (distance data)
 *   ESP -> MCX:  [0xBB][0x01][status]               (pet detection)
 *   ESP -> MCX:  [0xBB][0x10]                        (feed command)
 *   ESP -> MCX:  [0xBB][0x11]                        (play command)
 *   ESP -> MCX:  [0xBB][0x12]                        (stop command)
 *
 * Pins:
 *   GPIO 1  - Water level AO (ADC)
 *   GPIO 3  - MPU6050 INT (shock interrupt)
 *   GPIO 7  - Passive buzzer (LEDC PWM)
 *   GPIO 8  - I2C SDA (MPU6050)
 *   GPIO 9  - Laser emitter (digital out)
 *   GPIO 10 - I2C SCL (MPU6050)
 *   GPIO 11 - DHT11 data
 *   GPIO 17 - UART TX -> MCXC444 PTE23 (RX)
 *   GPIO 18 - UART RX <- MCXC444 PTE22 (TX)
 *
 * Board: ESP32S2 Dev Module, USB CDC On Boot: Enabled
 * Libraries:
 *   - DHT sensor library (Adafruit)
 *   - MPU6050 by Electronic Cats
 * =============================================================================
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <Wire.h>
#include <MPU6050.h>
#include <time.h>

/* ========================= CHANGE THESE ================================== */
#define WIFI_SSID "AndroidAP"
#define WIFI_PASSWORD "yecg5819"
#define DEVICE_ID "esp32s2-petpal"

/*
 * Firebase Realtime Database REST endpoint.
 * Example:
 *   https://your-project-id-default-rtdb.asia-southeast1.firebasedatabase.app
 *
 * For a quick demo, you can temporarily use permissive RTDB rules.
 * For locked-down rules, put a database secret or auth token in FIREBASE_AUTH.
 */
#define FIREBASE_DATABASE_URL "https://petpal-database-default-rtdb.asia-southeast1.firebasedatabase.app/"
#define FIREBASE_AUTH ""
#define FIREBASE_DEVICE_PATH "/petpal/devices/" DEVICE_ID

/* ========================= PINS ========================================== */
#define WATER_PIN 1    /* ADC - AO from water sensor              */
#define MPU_INT_PIN 3  /* MPU6050 INT output                      */
#define BUZZER_PIN 7   /* Passive buzzer - LEDC PWM               */
#define SDA_PIN 8      /* I2C SDA for MPU6050                     */
#define LASER_PIN 9    /* Laser emitter - digital out             */
#define SCL_PIN 10     /* I2C SCL for MPU6050                     */
#define DHT_PIN 11     /* DHT11 data                              */
#define UART_TX_PIN 17 /* UART TX -> MCXC444                      */
#define UART_RX_PIN 18 /* UART RX <- MCXC444                      */

/* ========================= TIMING ======================================== */
#define UART_BAUD 115200
#define WIFI_TIMEOUT_MS 15000
#define WIFI_RETRY_MS 10000
#define TELEMETRY_INTERVAL 500
#define COMMAND_POLL_MS 500
#define WATER_READ_MS 500
#define DHT_READ_MS 2000
#define STATUS_PRINT_MS 2000
#define SHOCK_DEBOUNCE_MS 500
#define NTP_TIMEOUT_MS 2000

/* ========================= PET DETECTION ================================= */
#define PET_NEAR_CM 30
#define PET_FAR_CM 60

/* ========================= WATER LEVELS ================================== */
#define WATER_EMPTY 50 /* 0   - 50   = empty  */
#define WATER_LOW 500  /* 50  - 500  = low    */
#define WATER_OK 2500  /* 500 - 2500 = ok     */
                       /* 2500+      = full   */

/* ========================= COMMANDS TO MCXC444 =========================== */
#define CMD_PET_STATUS 0x01
#define CMD_FEED 0x10
#define CMD_PLAY 0x11
#define CMD_STOP 0x12

/* ========================= DHT =========================================== */
DHT dht(DHT_PIN, DHT11);

/* ========================= MPU6050 ======================================= */
MPU6050 mpu;

/* ========================= STATE ========================================= */

/* Sensors */
float lastTemp = 0;
float lastHumidity = 0;
uint16_t lastWater = 0;
uint16_t lastDistanceCm = 999;

/* Shock (MPU6050 motion interrupt) */
volatile bool shockFlag = false;
volatile uint32_t shockCount = 0;
volatile unsigned long lastShockMs = 0;
bool shockLatched = false;

/* Pet detection */
bool petNear = false;
bool prevPetNear = false;

/* Actuators */
bool laserOn = false;
bool buzzerOn = false;
bool playServoMoving = false;
bool feederTriggered = false;

/* Buzzer timed */
bool buzzerTimedMode = false;
uint32_t buzzerOffAt = 0;

/* Events */
String lastEvent = "boot";
String lastFeedTs = "";
String lastPlayTs = "";

/* Timing */
unsigned long lastWaterRead = 0;
unsigned long lastDhtRead = 0;
unsigned long lastTelemetryPost = 0;
unsigned long lastCommandPoll = 0;
unsigned long lastStatusPrint = 0;
unsigned long lastWifiAttempt = 0;
unsigned long bootTime = 0;

/* UART parser state */
bool inBinaryPacket = false;
uint8_t binStep = 0;
uint8_t parseType = 0;
uint8_t parseHi = 0;

/* Text buffer for MCXC444 debug output */
char textBuf[128];
int textIdx = 0;

WiFiClientSecure firebaseClient;

/* ========================= WATER LEVEL =================================== */

String getWaterLevel(uint16_t raw)
{
    if (raw < WATER_EMPTY)
        return "empty";
    if (raw < WATER_LOW)
        return "low";
    if (raw < WATER_OK)
        return "ok";
    return "full";
}

/* ========================= SHOCK ISR (MPU6050 INT) ======================= */

void IRAM_ATTR shockISR()
{
    unsigned long now = millis();
    if ((now - lastShockMs) >= SHOCK_DEBOUNCE_MS)
    {
        lastShockMs = now;
        shockCount++;
        shockFlag = true;
    }
}

/* ========================= BUZZER ======================================== */

void buzzerTone(uint16_t freq, uint16_t durationMs)
{
    ledcWriteTone(BUZZER_PIN, freq);
    buzzerOn = true;
    if (durationMs > 0)
    {
        buzzerTimedMode = true;
        buzzerOffAt = millis() + durationMs;
    }
}

void buzzerOff()
{
    ledcWriteTone(BUZZER_PIN, 0);
    buzzerOn = false;
    buzzerTimedMode = false;
}

/* ========================= LASER ========================================= */

void setLaser(bool on)
{
    laserOn = on;
    digitalWrite(LASER_PIN, on ? HIGH : LOW);
}

/* ========================= UART TO MCXC444 =============================== */

void sendToMCX(uint8_t type)
{
    Serial1.write(0xBB);
    Serial1.write(type);
}

void sendToMCX(uint8_t type, uint8_t data)
{
    Serial1.write(0xBB);
    Serial1.write(type);
    Serial1.write(data);
}

void sendPetStatus(bool near)
{
    sendToMCX(CMD_PET_STATUS, near ? 0x01 : 0x00);
}

/* ========================= UART FROM MCXC444 ============================= */

void processSerial1()
{
    while (Serial1.available())
    {
        uint8_t b = Serial1.read();

        /* 0xAA starts a binary packet */
        if (b == 0xAA && !inBinaryPacket)
        {
            if (textIdx > 0)
            {
                textBuf[textIdx] = '\0';
                Serial0.println(textBuf);
                textIdx = 0;
            }
            inBinaryPacket = true;
            binStep = 0;
            continue;
        }

        /* Inside binary packet: [type][hi][lo] */
        if (inBinaryPacket)
        {
            switch (binStep)
            {
            case 0:
                parseType = b;
                binStep = 1;
                break;
            case 1:
                parseHi = b;
                binStep = 2;
                break;
            case 2:
                if (parseType == 0x01)
                {
                    lastDistanceCm = ((uint16_t)parseHi << 8) | b;

                    /* Pet detection with hysteresis */
                    if (lastDistanceCm < PET_NEAR_CM)
                    {
                        petNear = true;
                    }
                    else if (lastDistanceCm > PET_FAR_CM)
                    {
                        petNear = false;
                    }

                    sendPetStatus(petNear);
                }
                inBinaryPacket = false;
                break;
            }
            continue;
        }

        /* Text lines from MCXC444 PRINTF */
        if (b == '\n')
        {
            textBuf[textIdx] = '\0';
            if (textIdx > 0)
                Serial0.println(textBuf);
            textIdx = 0;
        }
        else if (b != '\r')
        {
            if (textIdx < 126)
                textBuf[textIdx++] = (char)b;
        }
    }
}

/* ========================= WIFI ========================================== */

void setupWiFi()
{
    if (WiFi.status() == WL_CONNECTED)
        return;

    Serial0.printf("[WIFI] Connecting to %s...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastWifiAttempt = millis();

    while (WiFi.status() != WL_CONNECTED &&
           (millis() - lastWifiAttempt) < WIFI_TIMEOUT_MS)
    {
        delay(100);
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial0.printf("[WIFI] Connected! IP: %s\n",
                       WiFi.localIP().toString().c_str());

        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        unsigned long start = millis();
        time_t now = 0;
        while (now < 1700000000 && (millis() - start) < NTP_TIMEOUT_MS)
        {
            delay(100);
            time(&now);
        }
        if (now >= 1700000000)
        {
            Serial0.println("[TIME] NTP synced.");
        }
        else
        {
            Serial0.println("[TIME] NTP not synced yet; Firebase server timestamp will still be used.");
        }
    }
    else
    {
        Serial0.println("[WIFI] Timeout. Will retry.");
    }
}

/* ========================= TIMESTAMP ===================================== */

String nowIso()
{
    time_t now = 0;
    time(&now);
    if (now >= 1700000000)
    {
        struct tm timeinfo;
        gmtime_r(&now, &timeinfo);
        char iso[25];
        strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
        return String(iso);
    }

    unsigned long sec = millis() / 1000;
    char buf[32];
    snprintf(buf, sizeof(buf), "2026-01-01T%02lu:%02lu:%02luZ",
             (sec / 3600) % 24, (sec / 60) % 60, sec % 60);
    return String(buf);
}

/* ========================= FIREBASE REST ================================= */

bool firebaseConfigured()
{
    return String(FIREBASE_DATABASE_URL).indexOf("YOUR_PROJECT_ID") < 0;
}

String firebaseUrl(const char *path)
{
    String base = FIREBASE_DATABASE_URL;
    while (base.endsWith("/"))
    {
        base.remove(base.length() - 1);
    }

    String url = base + path + ".json";
    if (String(FIREBASE_AUTH).length() > 0)
    {
        url += "?auth=" + String(FIREBASE_AUTH);
    }
    return url;
}

bool firebaseBegin(HTTPClient &http, const String &url)
{
    firebaseClient.setInsecure(); /* Demo-friendly TLS. Use a root CA for production. */
    return http.begin(firebaseClient, url);
}

/* ========================= TELEMETRY POST ================================ */

void postTelemetry()
{
    if (WiFi.status() != WL_CONNECTED)
        return;

    if (!firebaseConfigured())
    {
        Serial0.println("[FIREBASE] Set FIREBASE_DATABASE_URL before uploading telemetry.");
        return;
    }

    HTTPClient http;
    String url = firebaseUrl(FIREBASE_DEVICE_PATH "/telemetry");
    if (!firebaseBegin(http, url))
    {
        Serial0.println("[FIREBASE] Telemetry begin failed.");
        return;
    }
    http.addHeader("Content-Type", "application/json");

    unsigned long uptimeSec = (millis() - bootTime) / 1000;

    String json = "{";
    json += "\"deviceId\":\"" + String(DEVICE_ID) + "\",";
    json += "\"mode\":\"normal\",";
    json += "\"uptimeSec\":" + String(uptimeSec) + ",";

    if (lastTemp != 0 || lastHumidity != 0)
    {
        json += "\"temperatureC\":" + String(lastTemp, 1) + ",";
        json += "\"humidityPct\":" + String(lastHumidity, 1) + ",";
    }
    else
    {
        json += "\"temperatureC\":null,";
        json += "\"humidityPct\":null,";
    }

    json += "\"distanceCm\":" + String(lastDistanceCm) + ",";
    json += "\"shockDetected\":" + String(shockLatched ? "true" : "false") + ",";
    json += "\"waterLevelRaw\":" + String(lastWater) + ",";
    json += "\"waterLevel\":\"" + getWaterLevel(lastWater) + "\",";
    json += "\"laserOn\":" + String(laserOn ? "true" : "false") + ",";
    json += "\"playServoMoving\":" + String(playServoMoving ? "true" : "false") + ",";
    json += "\"feederTriggered\":" + String(feederTriggered ? "true" : "false") + ",";
    json += "\"buzzerOn\":" + String(buzzerOn ? "true" : "false") + ",";
    json += "\"lastEvent\":\"" + lastEvent + "\",";
    json += "\"lastFeedTs\":" + (lastFeedTs.length() > 0 ? ("\"" + lastFeedTs + "\"") : "null") + ",";
    json += "\"lastPlayTs\":" + (lastPlayTs.length() > 0 ? ("\"" + lastPlayTs + "\"") : "null") + ",";
    json += "\"updatedAt\":\"" + nowIso() + "\",";
    json += "\"updatedAtMs\":{\".sv\":\"timestamp\"}";
    json += "}";

    int code = http.PUT(json);
    if (code == 200)
    {
        shockLatched = false;
        feederTriggered = false;
    }
    else if (code > 0)
    {
        Serial0.printf("[FIREBASE] Telemetry HTTP %d: %s\n",
                       code, http.getString().c_str());
    }
    else
    {
        Serial0.printf("[FIREBASE] Telemetry fail: %s\n",
                       http.errorToString(code).c_str());
    }
    http.end();
}

/* ========================= COMMAND POLLING ================================ */

void ackCommand(String cmdId, bool success)
{
    if (WiFi.status() != WL_CONNECTED)
        return;

    if (!firebaseConfigured())
        return;

    HTTPClient http;
    String url = firebaseUrl(FIREBASE_DEVICE_PATH "/command");
    if (!firebaseBegin(http, url))
    {
        Serial0.println("[FIREBASE] ACK begin failed.");
        return;
    }
    http.addHeader("Content-Type", "application/json");

    String json = "{";
    json += "\"id\":\"" + cmdId + "\",";
    json += "\"status\":\"" + String(success ? "executed" : "failed") + "\",";
    json += "\"executedAt\":\"" + nowIso() + "\"";
    json += "}";

    int code = http.PATCH(json);

    if (code == 200)
    {
        Serial0.printf("[FIREBASE] ACK sent: %s\n", cmdId.c_str());
    }
    else
    {
        Serial0.printf("[FIREBASE] ACK fail HTTP %d\n", code);
    }
    http.end();
}

void pollCommands()
{
    if (WiFi.status() != WL_CONNECTED)
        return;

    if (!firebaseConfigured())
        return;

    HTTPClient http;
    String url = firebaseUrl(FIREBASE_DEVICE_PATH "/command");
    if (!firebaseBegin(http, url))
    {
        Serial0.println("[FIREBASE] Command poll begin failed.");
        return;
    }

    int code = http.GET();
    if (code != 200)
    {
        if (code > 0)
            Serial0.printf("[FIREBASE] Cmd poll HTTP %d\n", code);
        else
            Serial0.printf("[FIREBASE] Cmd poll fail: %s\n",
                           http.errorToString(code).c_str());
        http.end();
        return;
    }

    String body = http.getString();
    http.end();

    if (body == "null" || body.indexOf("\"status\":\"queued\"") < 0)
    {
        return;
    }

    String cmdId = String(millis());
    String cmdType = "";

    int idIdx = body.indexOf("\"id\":\"");
    if (idIdx >= 0)
    {
        idIdx += 6;
        int idEnd = body.indexOf("\"", idIdx);
        if (idEnd > idIdx)
            cmdId = body.substring(idIdx, idEnd);
    }

    int typeIdx = body.indexOf("\"type\":\"");
    if (typeIdx >= 0)
    {
        typeIdx += 8;
        int typeEnd = body.indexOf("\"", typeIdx);
        if (typeEnd > typeIdx)
            cmdType = body.substring(typeIdx, typeEnd);
    }

    if (cmdId.length() == 0 || cmdType.length() == 0)
    {
        Serial0.println("[FIREBASE] Failed to parse command");
        return;
    }

    Serial0.printf("[CMD] id=%s type=%s\n", cmdId.c_str(), cmdType.c_str());

    bool success = false;

    if (cmdType == "feed_now")
    {
        sendToMCX(CMD_FEED);
        buzzerTone(1000, 500);
        feederTriggered = true;
        lastFeedTs = nowIso();
        lastEvent = "feed_now";
        success = true;
        Serial0.println("[CMD] -> MCXC444: FEED");
    }
    else if (cmdType == "play_mode_toggle")
    {
        if (!playServoMoving)
        {
            setLaser(true);
            sendToMCX(CMD_PLAY);
            playServoMoving = true;
            lastPlayTs = nowIso();
            lastEvent = "play_start";
            Serial0.println("[CMD] -> MCXC444: PLAY ON + laser ON");
        }
        else
        {
            setLaser(false);
            sendToMCX(CMD_STOP);
            playServoMoving = false;
            lastEvent = "play_stop";
            Serial0.println("[CMD] -> MCXC444: PLAY OFF + laser OFF");
        }
        success = true;
    }

    ackCommand(cmdId, success);
}

/* ========================= SENSOR READING ================================ */

void readSensors()
{
    unsigned long now = millis();

    /* Shock (MPU6050 motion interrupt) */
    if (shockFlag)
    {
        noInterrupts();
        shockFlag = false;
        interrupts();
        shockLatched = true;
        lastEvent = "shock";
        buzzerTone(2400, 120);
        Serial0.printf("[SHOCK] Tap #%lu\n", shockCount);
    }

    /* Water */
    if (now - lastWaterRead >= WATER_READ_MS)
    {
        lastWaterRead = now;
        lastWater = analogRead(WATER_PIN);
    }

    /* DHT */
    if (now - lastDhtRead >= DHT_READ_MS)
    {
        lastDhtRead = now;
        float t = dht.readTemperature();
        float h = dht.readHumidity();
        if (!isnan(t) && !isnan(h))
        {
            lastTemp = t;
            lastHumidity = h;
        }
    }

    /* Buzzer auto-off */
    if (buzzerTimedMode && now >= buzzerOffAt)
    {
        buzzerOff();
    }

    /* Pet arrival/departure events */
    if (petNear != prevPetNear)
    {
        prevPetNear = petNear;
        if (petNear)
        {
            lastEvent = "pet_arrived";
            buzzerTone(1500, 300);
            Serial0.printf("[PET] *** DETECTED *** dist: %d cm\n",
                           lastDistanceCm);
        }
        else
        {
            lastEvent = "pet_left";
            Serial0.printf("[PET] Left. dist: %d cm\n", lastDistanceCm);
        }
    }
}

/* ========================= SETUP ========================================= */

void setup()
{
    Serial0.begin(115200);
    delay(3000);
    bootTime = millis();

    Serial0.println("\n========================================");
    Serial0.println("  PetPal ESP32-S2 v3");
    Serial0.println("  Sensors + Firebase + UART Bridge");
    Serial0.println("========================================\n");

    /* UART to MCXC444 */
    Serial1.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    Serial0.printf("[INIT] UART: TX=%d RX=%d\n", UART_TX_PIN, UART_RX_PIN);

    /* Water sensor */
    pinMode(WATER_PIN, INPUT);
    analogReadResolution(12);

    /* DHT11 */
    dht.begin();

    /* Buzzer */
    ledcAttach(BUZZER_PIN, 1000, 8);
    ledcWriteTone(BUZZER_PIN, 0);

    /* Laser */
    pinMode(LASER_PIN, OUTPUT);
    digitalWrite(LASER_PIN, LOW);

    /* MPU6050 */
    Wire.begin(SDA_PIN, SCL_PIN);
    mpu.initialize();

    if (mpu.testConnection())
    {
        Serial0.println("[MPU] Connected OK");
    }
    else
    {
        Serial0.println("[MPU] ERROR - check SDA/SCL wiring");
    }

    mpu.setMotionDetectionThreshold(10); /* Raise if too sensitive */
    mpu.setMotionDetectionDuration(1);
    mpu.setIntMotionEnabled(true);

    pinMode(MPU_INT_PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(MPU_INT_PIN), shockISR, RISING);

    Serial0.printf("[INIT] Water=%d DHT=%d Buzzer=%d Laser=%d\n",
                   WATER_PIN, DHT_PIN, BUZZER_PIN, LASER_PIN);
    Serial0.printf("[INIT] MPU6050 SDA=%d SCL=%d INT=%d\n",
                   SDA_PIN, SCL_PIN, MPU_INT_PIN);

    /* Self-test */
    Serial0.println("[TEST] Buzzer...");
    buzzerTone(1000, 200);
    delay(300);
    buzzerOff();

    Serial0.println("[TEST] Laser...");
    setLaser(true);
    delay(500);
    setLaser(false);

    setupWiFi();

    Serial0.printf("[INIT] Firebase: %s\n", FIREBASE_DATABASE_URL);
    Serial0.println("[INIT] Ready.\n");
}

/* ========================= LOOP ========================================== */

void loop()
{
    unsigned long now = millis();

    /* WiFi reconnect */
    if (WiFi.status() != WL_CONNECTED &&
        (now - lastWifiAttempt) >= WIFI_RETRY_MS)
    {
        setupWiFi();
    }

    /* UART from MCXC444 */
    processSerial1();

    /* Read local sensors */
    readSensors();

    /* PUT telemetry to Firebase */
    if (now - lastTelemetryPost >= TELEMETRY_INTERVAL)
    {
        lastTelemetryPost = now;
        postTelemetry();
    }

    /* Poll Firebase for dashboard commands */
    if (now - lastCommandPoll >= COMMAND_POLL_MS)
    {
        lastCommandPoll = now;
        pollCommands();
    }

    /* Status print */
    if (now - lastStatusPrint >= STATUS_PRINT_MS)
    {
        lastStatusPrint = now;
        Serial0.println("--- STATUS ---");
        Serial0.printf("  Dist:   %d cm\n", lastDistanceCm);
        Serial0.printf("  Pet:    %s\n", petNear ? "YES" : "no");
        Serial0.printf("  Temp:   %.1fC\n", lastTemp);
        Serial0.printf("  Humid:  %.1f%%\n", lastHumidity);
        Serial0.printf("  Water:  %d (%s)\n", lastWater,
                       getWaterLevel(lastWater).c_str());
        Serial0.printf("  Shock:  %lu\n", shockCount);
        Serial0.printf("  Laser:  %s\n", laserOn ? "ON" : "OFF");
        Serial0.printf("  Play:   %s\n", playServoMoving ? "YES" : "no");
        Serial0.printf("  WiFi:   %s\n", WiFi.status() == WL_CONNECTED
                                             ? "OK"
                                             : "DOWN");
        Serial0.printf("  Event:  %s\n", lastEvent.c_str());
        Serial0.println("--------------\n");
    }

    delay(10);
}
