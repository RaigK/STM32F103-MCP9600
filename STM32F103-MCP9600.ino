#include <Wire.h>
#include <STM32RTC.h>
#include "SparkFun_MCP9600.h" 
#include "ssd1306.h"

// --- Instanzen ---
STM32RTC& rtc = STM32RTC::getInstance();
TwoWire Wire1(PB7, PB6); 
MCP9600 sensor1;
MCP9600 sensor2;

// --- Pins ---
#define SENSOR1_ADDR 0x60
#define SENSOR2_ADDR 0x67
#define ENC_PHASE_A  PB3
#define ENC_PHASE_B  PB4
#define ENC_BUTTON   PA15 
#define BUZZER_PIN   PA3

// --- Zustände ---
enum Mode { CLOCK_DISP, MENU_NAV, EDIT_TIME, EDIT_DATE, EDIT_BUZZER, EDIT_SENSORS, VIEW_COLD_JUNC };
volatile Mode currentMode = CLOCK_DISP;

// --- Variablen ---
const char* dayNames[]  = { "??", "Mo", "Di", "Mi", "Do", "Fr", "Sa", "So" };
const char* typeNames[] = { "K", "J", "T", "N", "S", "E", "B", "R" };
const char* menuItems[] = { "<- Zurueck", "Zeit stellen", "Datum stellen", "Buzzer Setup", "Sensor Typen", "Cold Junction" };
SAppMenu menu;

volatile int8_t encoderDelta = 0;
volatile bool buttonPressed = false;
uint8_t editH, editM, editS, editD, editMo, editY;
uint8_t sensor1TypeIdx = 0; 
uint8_t sensor2TypeIdx = 0;
uint8_t cursorIdx = 0;

// Power-Management
uint32_t lastActivity = 0;
bool displayOn = true;
#define SLEEP_TIMEOUT 30000 // 30 Sekunden

bool buzzerEnabled = true;
uint16_t buzzerFreq = 2000;
uint8_t buzzerVol = 127;

// --- ZEICHEN-FUNKTIONEN (UI) ---
void drawEditTime() {
    ssd1306_clearScreen();
    ssd1306_printFixed(25, 10, "UHR STELLEN", STYLE_BOLD);
    char buf[20]; sprintf(buf, "%02d:%02d:%02d", editH, editM, editS);
    ssd1306_printFixed(40, 35, buf, STYLE_BOLD);
    ssd1306_printFixed(40 + (cursorIdx * 18), 45, "^^", STYLE_BOLD);
}

void drawEditDate() {
    ssd1306_clearScreen();
    ssd1306_printFixed(25, 10, "DATUM STELLEN", STYLE_BOLD);
    char buf[20]; sprintf(buf, "%02d.%02d.20%02d", editD, editMo, editY);
    ssd1306_printFixed(30, 35, buf, STYLE_BOLD);
    uint8_t xPos = 30 + (cursorIdx * 18); if (cursorIdx == 2) xPos += 12;
    ssd1306_printFixed(xPos, 45, "^^", STYLE_BOLD);
}

void drawEditBuzzer() {
    ssd1306_clearScreen();
    ssd1306_printFixed(25, 5, "BUZZER SETUP", STYLE_BOLD);
    char buf[25];
    sprintf(buf, "Aktiv: %s", buzzerEnabled ? "JA" : "NEIN");
    ssd1306_printFixed(10, 25, buf, cursorIdx == 0 ? STYLE_ITALIC : STYLE_NORMAL);
    sprintf(buf, "Pitch: %d Hz", buzzerFreq);
    ssd1306_printFixed(10, 40, buf, cursorIdx == 1 ? STYLE_ITALIC : STYLE_NORMAL);
    sprintf(buf, "Puls:  %d", buzzerVol);
    ssd1306_printFixed(10, 55, buf, cursorIdx == 2 ? STYLE_ITALIC : STYLE_NORMAL);
}

void playClick() {
    if (buzzerEnabled && buzzerVol > 0) {
        analogWriteFrequency(buzzerFreq);
        uint8_t dutyValue = map(buzzerVol, 0, 255, 255, 127);
        analogWrite(BUZZER_PIN, dutyValue);
        delay(25); 
        analogWrite(BUZZER_PIN, 255);
        digitalWrite(BUZZER_PIN, HIGH);
    }
}

void wakeUp() {
    if (!displayOn) {
        ssd1306_displayOn();
        displayOn = true;
    }
    lastActivity = millis();
}

void encoderISR() {
    static unsigned long lastEncTime = 0;
    if (millis() - lastEncTime > 4) {
        if (digitalRead(ENC_PHASE_A) != digitalRead(ENC_PHASE_B)) encoderDelta++;
        else encoderDelta--;
    }
    lastEncTime = millis();
}

void buttonISR() {
    static unsigned long lastBtnTime = 0;
    if (millis() - lastBtnTime > 250) buttonPressed = true;
    lastBtnTime = millis();
}

// --- HILFSFUNKTIONEN ---
void loadRtcToEdit() {
    uint8_t dow; uint32_t sub;
    rtc.getTime(&editH, &editM, &editS, &sub);
    rtc.getDate(&dow, &editD, &editMo, &editY);
}

// --- DISPLAY UPDATES ---
void drawColdJunction() {
    //ssd1306_clearScreen();
    ssd1306_printFixed(20, 0, "COLD JUNCTION", STYLE_BOLD);
    ssd1306_drawLine(0, 15, 127, 15);
    char buf[25];
    sprintf(buf, "Chip 1: %.2f C", sensor1.getAmbientTemp());
    ssd1306_printFixed(10, 25, buf, STYLE_NORMAL);
    sprintf(buf, "Chip 2: %.2f C", sensor2.getAmbientTemp());
    ssd1306_printFixed(10, 40, buf, STYLE_NORMAL);
}

void drawEditSensorTypes() {
    ssd1306_clearScreen();
    ssd1306_printFixed(20, 5, "SENSOR TYPEN", STYLE_BOLD);
    char buf[25];
    sprintf(buf, "%s S1: Typ %s", (cursorIdx == 0 ? ">" : " "), typeNames[sensor1TypeIdx]);
    ssd1306_printFixed(10, 25, buf, cursorIdx == 0 ? STYLE_ITALIC : STYLE_NORMAL);
    sprintf(buf, "%s S2: Typ %s", (cursorIdx == 1 ? ">" : " "), typeNames[sensor2TypeIdx]);
    ssd1306_printFixed(10, 40, buf, cursorIdx == 1 ? STYLE_ITALIC : STYLE_NORMAL);
}

// --- HANDLER ---
void handleTimeSetting(int8_t move, bool clicked) {
    if (move != 0) {
        if (cursorIdx == 0) editH = (editH + move + 24) % 24;
        else if (cursorIdx == 1) editM = (editM + move + 60) % 60;
        else if (cursorIdx == 2) editS = (editS + move + 60) % 60;
        drawEditTime();
    }
    if (clicked) {
        if (++cursorIdx > 2) { 
            rtc.setTime(editH, editM, editS); 
            currentMode = MENU_NAV; ssd1306_clearScreen(); ssd1306_showMenu(&menu); 
        } else drawEditTime();
    }
}

void handleDateSetting(int8_t move, bool clicked) {
    if (move != 0) {
        if (cursorIdx == 0) editD = (editD + move == 0) ? 31 : (editD + move > 31 ? 1 : editD + move);
        else if (cursorIdx == 1) editMo = (editMo + move == 0) ? 12 : (editMo + move > 12 ? 1 : editMo + move);
        else if (cursorIdx == 2) editY = (editY + move + 100) % 100;
        drawEditDate();
    }
    if (clicked) {
        if (++cursorIdx > 2) { 
            rtc.setDate(1, editD, editMo, editY); 
            currentMode = MENU_NAV; ssd1306_clearScreen(); ssd1306_showMenu(&menu); 
        } else drawEditDate();
    }
}

void handleBuzzerSetting(int8_t move, bool clicked) {
    if (move != 0) {
        if (cursorIdx == 0) buzzerEnabled = !buzzerEnabled;
        else if (cursorIdx == 1) buzzerFreq = constrain(buzzerFreq + (move * 100), 200, 5000);
        else if (cursorIdx == 2) buzzerVol = constrain(buzzerVol + (move * 10), 0, 255);
        drawEditBuzzer();
        if (cursorIdx > 0) playClick();
    }
    if (clicked) {
        if (++cursorIdx > 2) { currentMode = MENU_NAV; ssd1306_clearScreen(); ssd1306_showMenu(&menu); }
        else drawEditBuzzer();
    }
}

void handleSensorTypeSetting(int8_t move, bool clicked) {
    if (move != 0) {
        if (cursorIdx == 0) { 
            sensor1TypeIdx = (sensor1TypeIdx + move + 8) % 8; 
            // FIX: Explizites Casting auf Thermocouple_Type
            sensor1.setThermocoupleType((Thermocouple_Type)sensor1TypeIdx); 
        } else { 
            sensor2TypeIdx = (sensor2TypeIdx + move + 8) % 8; 
            sensor2.setThermocoupleType((Thermocouple_Type)sensor2TypeIdx); 
        }
        drawEditSensorTypes();
    }
    if (clicked) {
        if (++cursorIdx > 1) { currentMode = MENU_NAV; ssd1306_clearScreen(); ssd1306_showMenu(&menu); }
        else drawEditSensorTypes();
    }
}

// (handleTimeSetting, handleDateSetting, handleBuzzerSetting wie zuvor...)
// [Platzhalter für Zeit/Datum/Buzzer Handler aus dem vorherigen Code]

void setup() {
    delay(500);
    RCC->APB2ENR |= RCC_APB2ENR_AFIOEN;
    AFIO->MAPR |= AFIO_MAPR_SWJ_CFG_JTAGDISABLE;

    Wire.setSDA(PB11); Wire.setSCL(PB10);
    sh1106_128x64_i2c_init();
    ssd1306_setFixedFont(ssd1306xled_font6x8);
    
    // Strom sparen: Kontrast reduzieren (1-255)
    ssd1306_setContrast(200); 
    ssd1306_fillScreen(0x00);

    rtc.setClockSource(STM32RTC::LSE_CLOCK);
    rtc.begin();

    Wire1.begin();
    sensor1.begin(SENSOR1_ADDR, Wire1);
    sensor2.begin(SENSOR2_ADDR, Wire1);

    pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, HIGH);
    pinMode(ENC_PHASE_A, INPUT_PULLUP); pinMode(ENC_PHASE_B, INPUT_PULLUP); pinMode(ENC_BUTTON,  INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(ENC_PHASE_A), encoderISR, FALLING);
    attachInterrupt(digitalPinToInterrupt(ENC_BUTTON),  buttonISR,  FALLING);

    ssd1306_createMenu(&menu, menuItems, sizeof(menuItems) / sizeof(char *));
    lastActivity = millis();
}

void loop() {
    noInterrupts();
    int8_t move = encoderDelta; encoderDelta = 0;
    bool clicked = buttonPressed; buttonPressed = false;
    interrupts();

    // Bei jeder Interaktion: Display wecken / Timer reset
    if (move != 0 || clicked) {
        wakeUp();
        if (clicked) playClick();
    }

    // Energiesparmodus: Display aus nach Inaktivität
    if (displayOn && (millis() - lastActivity > SLEEP_TIMEOUT)) {
        ssd1306_setContrast(100); 
    //    ssd1306_displayOff();
        displayOn = false;
    }

    //if (!displayOn) return; // Wenn Display aus, CPU-Last minimieren

    switch (currentMode) {
        case CLOCK_DISP: {
            static uint32_t lastUpd = 0;
            if (millis() - lastUpd > 1000) {
                uint8_t h, m, s, d, mo, y, dow; uint32_t sub;
                rtc.getTime(&h, &m, &s, &sub); rtc.getDate(&dow, &d, &mo, &y);
                //ssd1306_clearScreen();
                char b[30];
                sprintf(b, "%02d:%02d:%02d",  h, m, s);
                ssd1306_printFixed(39, 0, b, STYLE_NORMAL);
                sprintf(b, "%s  %02d.%02d.20%02d", dayNames[dow], d, mo, y);
                ssd1306_printFixed(22, 12, b, STYLE_NORMAL);
                ssd1306_drawLine(0, 24, 127, 24);
                sprintf(b, "S1: %.1f degC (%s)", sensor1.getThermocoupleTemp(), typeNames[sensor1TypeIdx]);
                ssd1306_printFixed(0, 32, b, STYLE_NORMAL);
                sprintf(b, "S2: %.1f degC (%s)", sensor2.getThermocoupleTemp(), typeNames[sensor2TypeIdx]);
                ssd1306_printFixed(0, 48, b, STYLE_NORMAL);
                lastUpd = millis();
            }
            if (clicked || move != 0) { currentMode = MENU_NAV; ssd1306_clearScreen(); ssd1306_showMenu(&menu); }
            break;
        }

        case MENU_NAV:
            if (move > 0) { ssd1306_menuDown(&menu); ssd1306_updateMenu(&menu); }
            else if (move < 0) { ssd1306_menuUp(&menu); ssd1306_updateMenu(&menu); }
            else if (clicked) {
                uint8_t sel = ssd1306_menuSelection(&menu);
                if (sel == 0) { currentMode = CLOCK_DISP; ssd1306_clearScreen(); }
                else if (sel == 1) { loadRtcToEdit(); currentMode = EDIT_TIME; drawEditTime(); }
                else if (sel == 2) { loadRtcToEdit(); currentMode = EDIT_DATE; drawEditDate(); }
                else if (sel == 3) { currentMode = EDIT_BUZZER; drawEditBuzzer(); }
                else if (sel == 4) { cursorIdx = 0; currentMode = EDIT_SENSORS; drawEditSensorTypes(); }
                else if (sel == 5) { ssd1306_clearScreen(); currentMode = VIEW_COLD_JUNC; drawColdJunction(); }
                // Weitere Cases...
            }
            break;

        case EDIT_TIME:    handleTimeSetting(move, clicked); break;
        case EDIT_DATE:    handleDateSetting(move, clicked); break;
        case EDIT_BUZZER:  handleBuzzerSetting(move, clicked); break;
        case EDIT_SENSORS: handleSensorTypeSetting(move, clicked); break;

        case VIEW_COLD_JUNC: {
            static uint32_t lastCJ = 0;
            if (millis() - lastCJ > 1000) { drawColdJunction(); lastCJ = millis(); }
            if (clicked) { currentMode = MENU_NAV; ssd1306_clearScreen(); ssd1306_showMenu(&menu); }
            break;
        }

        // Weitere Cases...
    }
}