#include <mcp_can.h>
#include <SPI.h>
#include <MCUFRIEND_kbv.h>

/*
 * Eltek Flatpack2 CAN Bus power control v1 19/10/25    Eliot Mansfield  www.mez.co.uk
 * What I use it for:  Allows me to use my petrol generator to charge up my Pylontech USC3000 batteries via 48V DC (In case of prolonged grid failure /shtf)
 *
 * Experimental - Use at your own risk and modify accordingly.  Mains Voltages and Lithium Batteries should be treated with extreme care.
 * Known issue: It keeps constantly sending the permanent startup voltage back to the powersupply - which could eventually wear out the flash, code needs a spin to prevent that.
 *	Workaround - leave the display in the voltage/current set menu to avoid constant updating.
 *
 * This was "vibe coded" using Claude AI - I'm not a developer therefore I reccomend eye protection for any professional coders looking through this.
 *
 * Hardware: 
 * Eltek FLATPACK2 48/3000 HE REV 6  48V 3kW DC power supply (should work with most revisions)
 * Eltek Flatpack 2HE Power Board (ebay seller cool_as_ice0)
 * Arduino Mega2560 (mainly because i had it lying around along with the display)
 * KY-40 Rotary Encoder
 * Generic ebay 15-72v To 12V DC-DC Step Down Buck Converter to power the Arduino
 * C20 Panel Mount Plug Adapter (16A)
 * 50W 50Ohm pre-charge resistor (prevents arcing on the battery isolator on initial connection)
 * Vandal Resistant Waterproof Off-(On) 12mm Push Button Momentary Switch 2A SPST - Switch Electronics ebay shop
 * Durite 0-605-1 Battery isolator (note NOT rated for 48V...)
 * AMPHENOL RadSok SurLok Plus™ 5.7mm Connectors (Pylontech style connectors) - https://servtec.co.uk/
 *
 *
 * WIRING INSTRUCTIONS FOR ARDUINO MEGA 2560
 * MCP2515 CAN Shield:
 * - Plugs directly onto Arduino Mega (uses standard SPI pins)
 * - SCK  -> Pin 52 (hardware SPI)
 * - MOSI -> Pin 51 (hardware SPI)
 * - MISO -> Pin 50 (hardware SPI)
 * - CS   -> Pin 10 (fixed on shield)
 * - INT  -> Pin 2 (not used in this code due to conflict with the tft display)
 * 
 * 3.5" TFT LCD Shield (Parallel):
 * - Plugs on top of CAN shield
 * - Uses digital pins for 8-bit parallel data bus
 * - Auto-detected by MCUFRIEND_kbv library
 * 
 * KY-040 Rotary Encoder (use extended Mega pins above shields):
 * - CLK -> Pin 22 (easy access above shields)
 * - DT  -> Pin 23 (easy access above shields)
 * - SW  -> Pin 24 (easy access above shields - push button)
 * - +   -> 5V
 * - GND -> GND
 * 
 * Note: Pins 22-24 don't have hardware interrupts, so we'll use polling
 *       for the encoder instead of interrupts.
 */

// TFT Color definitions
#define BLACK   0x0000
#define BLUE    0x001F
#define RED     0xF800
#define GREEN   0x07E0
#define CYAN    0x07FF
#define MAGENTA 0xF81F
#define YELLOW  0xFFE0
#define WHITE   0xFFFF
#define ORANGE  0xFD20
#define DARKGREY 0x7BEF
#define LIGHTGREY 0xC618

// Voltage and soc levels for Pylontech Batteries: 52.14 is 100%, 50.76 is 90%, 47.5 is 10%
#define DEFAULT_VOLTAGE 4750        // Default voltage in centivolts (47.50V)
#define DEFAULT_CURRENT 200          // Default current limit in deciamps (20.0A)
#define DEFAULT_OVP_VOLTAGE 5200    // Over-voltage protection in centivolts
#define DEFAULT_WALKIN_TIME_LONG 1   // 1 = 60 second walk-in, 0 = 5 second walk-in

const int CAN_CS_PIN = 10;

// Rotary encoder pins (using extended Mega pins above shields)
const int ENCODER_CLK = 22;
const int ENCODER_DT = 23;
const int ENCODER_SW = 24;

const char *alarms0Strings[] = {"OVS_LOCK_OUT", "MOD_FAIL_PRIMARY", "MOD_FAIL_SECONDARY", "HIGH_MAINS", "LOW_MAINS", "HIGH_TEMP", "LOW_TEMP", "CURRENT_LIMIT"};
const char *alarms1Strings[] = {"INTERNAL_VOLTAGE", "MODULE_FAIL", "MOD_FAIL_SECONDARY", "FAN1_SPEED_LOW", "FAN2_SPEED_LOW", "SUB_MOD1_FAIL", "FAN3_SPEED_LOW", "INNER_VOLT"};

MCP_CAN CAN(CAN_CS_PIN);
MCUFRIEND_kbv tft;

bool serialNumberReceived = false;
bool settingsApplied = false;
bool permanentVoltageSet = false;
uint8_t serialNumber[8] = {0};
int messagesReceived = 0;

unsigned long lastLogInTime = 0;
unsigned long lastDisplayUpdate = 0;
const unsigned long DISPLAY_UPDATE_INTERVAL = 1000;  // Update display every 1 second

// Adjustable settings
int currentVoltage = DEFAULT_VOLTAGE;
int currentLimit = DEFAULT_CURRENT;
int ovpVoltage = DEFAULT_OVP_VOLTAGE;
bool walkinTimeLong = DEFAULT_WALKIN_TIME_LONG;

// Menu system
enum MenuMode {
    MODE_NORMAL,
    MODE_MENU,
    MODE_EDIT_VOLTAGE,
    MODE_EDIT_CURRENT,
    MODE_EDIT_WALKIN
};

MenuMode currentMode = MODE_NORMAL;
MenuMode lastMode = MODE_NORMAL;
int menuSelection = 0;
int lastMenuSelection = -1;  // Track last menu selection
const int MENU_ITEMS = 4;  // Added EXIT option

// Encoder variables
int lastCLKstate = HIGH;
int encoderPos = 0;
int lastEncoderPos = 0;
bool buttonPressed = false;
unsigned long lastButtonPress = 0;
const unsigned long DEBOUNCE_DELAY = 200;

// Store latest status data
struct {
    int intakeTemp;
    float current;
    float outputVoltage;
    int inputVoltage;
    int outputTemp;
    uint32_t statusID;
    bool hasData;
    String statusText;
    bool hasWarning;
    bool hasAlarm;
} latestStatus = {0, 0, 0, 0, 0, 0, false, "", false, false};

void readEncoder() {
    // Read encoder rotation
    int currentCLKstate = digitalRead(ENCODER_CLK);
    
    if (currentCLKstate != lastCLKstate && currentCLKstate == LOW) {
        if (digitalRead(ENCODER_DT) == HIGH) {
            encoderPos++;
        } else {
            encoderPos--;
        }
    }
    lastCLKstate = currentCLKstate;
    
    // Read button with debouncing
    if (digitalRead(ENCODER_SW) == LOW) {
        unsigned long currentTime = millis();
        if (currentTime - lastButtonPress > DEBOUNCE_DELAY) {
            buttonPressed = true;
            lastButtonPress = currentTime;
        }
    }
}

void setup() {
    Serial.begin(115200);

    // Initialize encoder pins
    pinMode(ENCODER_CLK, INPUT_PULLUP);
    pinMode(ENCODER_DT, INPUT_PULLUP);
    pinMode(ENCODER_SW, INPUT_PULLUP);
    lastCLKstate = digitalRead(ENCODER_CLK);

    // Initialize TFT
    uint16_t identifier = tft.readID();
    if (identifier == 0) identifier = 0x9341; // Default to ILI9341 if can't read
    tft.begin(identifier);
    tft.setRotation(1); // Landscape mode
    tft.fillScreen(BLACK);
    
    // Display startup message
    drawStartupScreen();

    pinMode(CAN_CS_PIN, OUTPUT);

    if (CAN.begin(MCP_ANY, CAN_125KBPS, MCP_16MHZ) == CAN_OK) {
        Serial.println("MCP2515 initialized successfully.");
        updateStatusMessage("CAN Bus OK", GREEN);
    } else {
        Serial.println("Failed to initialize MCP2515. Halting.");
        updateStatusMessage("CAN FAIL!", RED);
        while(1);
    }

    CAN.setMode(MCP_NORMAL);
    delay(1000);
    drawMainScreen();
}

void drawStartupScreen() {
    tft.fillScreen(BLACK);
    tft.setTextSize(3);
    tft.setTextColor(CYAN);
    tft.setCursor(80, 100);
    tft.print("ELTEK");
    tft.setCursor(40, 140);
    tft.print("FLATPACK 2");
    tft.setTextSize(2);
    tft.setTextColor(WHITE);
    tft.setCursor(80, 200);
    tft.print("Controller");
}

void drawMainScreen() {
    tft.fillScreen(BLACK);
    
    // Draw header
    tft.fillRect(0, 0, 480, 40, BLUE);
    tft.setTextSize(2);
    tft.setTextColor(WHITE);
    tft.setCursor(120, 12);
    tft.print("ELTEK FLATPACK 2");
    
    // Draw labels
    tft.setTextSize(2);
    tft.setTextColor(YELLOW);
    
    tft.setCursor(10, 60);
    tft.print("Output Voltage:");
    
    tft.setCursor(10, 95);
    tft.print("Current:");
    
    tft.setCursor(10, 130);
    tft.print("Power:");
    
    tft.setCursor(10, 165);
    tft.print("Input Voltage:");
    
    tft.setCursor(10, 200);
    tft.print("Intake Temp:");
    
    tft.setCursor(10, 235);
    tft.print("Output Temp:");
    
    tft.setCursor(10, 270);
    tft.print("Status:");
}

void drawMenuScreen() {
    tft.fillScreen(BLACK);
    
    // Draw header
    tft.fillRect(0, 0, 480, 40, BLUE);
    tft.setTextSize(2);
    tft.setTextColor(WHITE);
    tft.setCursor(160, 12);
    tft.print("SETTINGS");
    
    // Draw all menu items
    for (int i = 0; i < MENU_ITEMS; i++) {
        drawMenuItem(i, false);
    }
    
    // Highlight the current selection
    drawMenuItem(menuSelection, true);
    lastMenuSelection = menuSelection;
    
    // Instructions
    tft.setTextSize(1);
    tft.setTextColor(LIGHTGREY);
    tft.setCursor(10, 290);
    tft.print("Rotate to select, Press to edit/confirm");
}

void drawMenuItem(int index, bool selected) {
    int yPos = 70 + (index * 50);
    
    // Draw background
    if (selected) {
        tft.fillRect(0, yPos - 5, 480, 45, DARKGREY);
        tft.setTextColor(YELLOW);
    } else {
        tft.fillRect(0, yPos - 5, 480, 45, BLACK);
        tft.setTextColor(WHITE);
    }
    
    // Draw text
    tft.setTextSize(2);
    tft.setCursor(20, yPos);
    
    switch(index) {
        case 0:
            tft.print("Set Voltage: ");
            tft.print(currentVoltage / 100.0, 2);
            tft.print("V");
            break;
        case 1:
            tft.print("Set Current: ");
            tft.print(currentLimit / 10.0, 1);
            tft.print("A");
            break;
        case 2:
            tft.print("Walk-in: ");
            tft.print(walkinTimeLong ? "60 sec" : "5 sec");
            break;
        case 3:
            tft.print("EXIT to Normal View");
            break;
    }
}

void updateMenuSelection() {
    if (lastMenuSelection != menuSelection) {
        // Redraw old selection as unselected
        if (lastMenuSelection >= 0) {
            drawMenuItem(lastMenuSelection, false);
        }
        // Draw new selection as selected
        drawMenuItem(menuSelection, true);
        lastMenuSelection = menuSelection;
    }
}

void drawEditScreen(String label, String value, String unit) {
    tft.fillScreen(BLACK);
    
    // Draw header
    tft.fillRect(0, 0, 480, 40, ORANGE);
    tft.setTextSize(2);
    tft.setTextColor(WHITE);
    tft.setCursor(140, 12);
    tft.print("EDIT MODE");
    
    // Draw editing item
    tft.setTextSize(3);
    tft.setTextColor(YELLOW);
    tft.setCursor(40, 100);
    tft.print(label);
    
    // Draw value in large text (will be updated separately)
    updateEditValue(value, unit);
    
    // Instructions
    tft.setTextSize(1);
    tft.setTextColor(LIGHTGREY);
    tft.setCursor(100, 290);
    tft.print("Rotate to adjust, Press to confirm");
}

void updateEditValue(String value, String unit) {
    // Clear only the value area
    tft.fillRect(50, 160, 380, 60, BLACK);
    
    // Draw value in large text
    tft.setTextSize(5);
    tft.setTextColor(GREEN);
    tft.setCursor(100, 160);
    tft.print(value);
    tft.print(" ");
    tft.print(unit);
}

void updateStatusMessage(String message, uint16_t color) {
    tft.fillRect(0, 300, 480, 20, BLACK);
    tft.setTextSize(1);
    tft.setTextColor(color);
    tft.setCursor(10, 305);
    tft.print(message);
}

void updateDisplayValue(int x, int y, String value, uint16_t color) {
    // Clear the value area
    tft.fillRect(x, y, 200, 25, BLACK);
    
    // Draw new value
    tft.setTextSize(2);
    tft.setTextColor(color);
    tft.setCursor(x, y);
    tft.print(value);
}

void updateDisplay() {
    if (currentMode != MODE_NORMAL) return;
    if (!latestStatus.hasData) return;
    
    // Update voltage
    String voltageStr = String(latestStatus.outputVoltage, 2) + " V";
    uint16_t voltColor = (latestStatus.outputVoltage < 46.0) ? RED : GREEN;
    updateDisplayValue(260, 60, voltageStr, voltColor);
    
    // Update current
    String currentStr = String(latestStatus.current, 1) + " A";
    uint16_t currentColor = (latestStatus.current > (currentLimit / 10.0) * 0.9) ? ORANGE : GREEN;
    updateDisplayValue(260, 95, currentStr, currentColor);
    
    // Update power (voltage x current)
    float power = latestStatus.outputVoltage * latestStatus.current;
    String powerStr = String(power, 0) + " W";
    uint16_t powerColor = (power > 1000) ? GREEN : WHITE;
    updateDisplayValue(260, 130, powerStr, powerColor);
    
    // Update input voltage
    String inputStr = String(latestStatus.inputVoltage) + " V";
    updateDisplayValue(260, 165, inputStr, WHITE);
    
    // Update intake temp
    String intakeTempStr = String(latestStatus.intakeTemp) + " C";
    uint16_t tempColor = (latestStatus.intakeTemp > 50) ? ORANGE : WHITE;
    updateDisplayValue(260, 200, intakeTempStr, tempColor);
    
    // Update output temp
    String outputTempStr = String(latestStatus.outputTemp) + " C";
    uint16_t outTempColor = (latestStatus.outputTemp > 60) ? ORANGE : WHITE;
    updateDisplayValue(260, 235, outputTempStr, outTempColor);
    
    // Update status text
    tft.fillRect(120, 270, 350, 25, BLACK);
    tft.setTextSize(2);
    uint16_t statusColor = WHITE;
    if (latestStatus.hasAlarm) statusColor = RED;
    else if (latestStatus.hasWarning) statusColor = ORANGE;
    else if (latestStatus.statusText.indexOf("Normal") >= 0) statusColor = GREEN;
    
    tft.setTextColor(statusColor);
    tft.setCursor(120, 270);
    tft.print(latestStatus.statusText);
    
    // Show set voltage/current in bottom corner
    tft.fillRect(350, 300, 130, 20, BLACK);
    tft.setTextSize(1);
    tft.setTextColor(CYAN);
    tft.setCursor(350, 305);
    tft.print("Set:");
    tft.print(currentVoltage / 100.0, 1);
    tft.print("V ");
    tft.print(currentLimit / 10.0, 1);
    tft.print("A");
}

void handleEncoder() {
    // Read encoder state
    readEncoder();
    
    if (encoderPos == lastEncoderPos && !buttonPressed) return;
    
    // Handle encoder rotation
    if (encoderPos != lastEncoderPos) {
        int diff = encoderPos - lastEncoderPos;
        lastEncoderPos = encoderPos;
        
        switch(currentMode) {
            case MODE_NORMAL:
                // Do nothing in normal mode
                break;
                
            case MODE_MENU:
                menuSelection += diff;
                if (menuSelection < 0) menuSelection = 0;
                if (menuSelection >= MENU_ITEMS) menuSelection = MENU_ITEMS - 1;
                updateMenuSelection();
                break;
                
            case MODE_EDIT_VOLTAGE:
                currentVoltage += (diff * 10); // Adjust by 0.1V increments
                if (currentVoltage < 4750) currentVoltage = 4750; // Min 47.5V
                if (currentVoltage > 5100) currentVoltage = 5100; // Max 51V
                updateEditValue(String(currentVoltage / 100.0, 2), "V");
                break;
                
            case MODE_EDIT_CURRENT:
                currentLimit += (diff * 10); // Adjust by 1A increments
                if (currentLimit < 200) currentLimit = 200; // Min 20A
                if (currentLimit > 500) currentLimit = 500; // Max 50A
                updateEditValue(String(currentLimit / 10.0, 1), "A");
                break;
                
            case MODE_EDIT_WALKIN:
                if (diff != 0) {
                    walkinTimeLong = !walkinTimeLong;
                    updateEditValue(walkinTimeLong ? "60" : "5", "sec");
                }
                break;
        }
    }
    
    // Handle button press
    if (buttonPressed) {
        switch(currentMode) {
            case MODE_NORMAL:
                // Enter menu mode
                currentMode = MODE_MENU;
                lastMode = MODE_NORMAL;
                menuSelection = 0;
                drawMenuScreen();
                break;
                
            case MODE_MENU:
                // Enter edit mode for selected item or exit
                switch(menuSelection) {
                    case 0:
                        currentMode = MODE_EDIT_VOLTAGE;
                        lastMode = MODE_MENU;
                        drawEditScreen("Voltage", String(currentVoltage / 100.0, 2), "V");
                        break;
                    case 1:
                        currentMode = MODE_EDIT_CURRENT;
                        lastMode = MODE_MENU;
                        drawEditScreen("Current", String(currentLimit / 10.0, 1), "A");
                        break;
                    case 2:
                        currentMode = MODE_EDIT_WALKIN;
                        lastMode = MODE_MENU;
                        drawEditScreen("Walk-in", walkinTimeLong ? "60" : "5", "sec");
                        break;
                    case 3:
                        // EXIT option - return to normal mode
                        currentMode = MODE_NORMAL;
                        lastMode = MODE_MENU;
                        drawMainScreen();
                        break;
                }
                break;
                
            case MODE_EDIT_VOLTAGE:
            case MODE_EDIT_CURRENT:
            case MODE_EDIT_WALKIN:
                // Confirm edit and return to menu
                currentMode = MODE_MENU;
                lastMode = MODE_EDIT_VOLTAGE; // Any edit mode
                settingsApplied = false; // Force re-application of settings
                applySettings();
                drawMenuScreen();
                break;
        }
        
        buttonPressed = false;
    }
}

void printMessage(uint32_t rxID, uint8_t len, uint8_t rxBuf[]) {
    char output[256];
    snprintf(output, 256, "ID: 0x%.8lX Length: %1d Data:", rxID, len);
    Serial.print(output);
    for (int i = 0; i < len; ++i) {
        snprintf(output, 256, " 0x%.2X", rxBuf[i]);
        Serial.print(output);
    }
    Serial.println();
}

void logIn() {
    CAN.sendMsgBuf(0x05004804, 1, 8, serialNumber);
}

void applySettings() {
    Serial.println("--------");
    Serial.println("Applying voltage and current settings.");
    Serial.println("--------");

    // Apply temporary settings for immediate effect (with walk-in)
    uint8_t settingsBuf[8] = {
        (uint8_t)(currentLimit & 0xFF),
        (uint8_t)((currentLimit >> 8) & 0xFF),
        (uint8_t)(currentVoltage & 0xFF),
        (uint8_t)((currentVoltage >> 8) & 0xFF),
        (uint8_t)(currentVoltage & 0xFF),
        (uint8_t)((currentVoltage >> 8) & 0xFF),
        (uint8_t)(ovpVoltage & 0xFF),
        (uint8_t)((ovpVoltage >> 8) & 0xFF)
    };

    uint32_t canID = walkinTimeLong ? 0x05FF4005 : 0x05FF4004;
    CAN.sendMsgBuf(canID, 1, 8, settingsBuf);
    delay(100);

    // Write to PERMANENT startup voltage register ONLY ONCE
    if (!permanentVoltageSet) {
        Serial.println("Writing PERMANENT startup voltage (once only)...");
        
        // Format: {0x29, 0x15, 0x00, voltage_low, voltage_high}
        uint8_t voltageSetTxBuf[5] = { 
            0x29, 
            0x15, 
            0x00, 
            (uint8_t)(DEFAULT_VOLTAGE & 0xFF), 
            (uint8_t)((DEFAULT_VOLTAGE >> 8) & 0xFF) 
        };
        
        CAN.sendMsgBuf(0x05019C00, 1, 5, voltageSetTxBuf);
        permanentVoltageSet = true;
        
        Serial.print("Permanent startup voltage set to: ");
        Serial.print(DEFAULT_VOLTAGE / 100.0);
        Serial.println("V (via 0x05019C00) - WRITTEN TO EEPROM");
    }

    Serial.print("Current limit set to: ");
    Serial.print(currentLimit / 10.0);
    Serial.println("A");
    Serial.print("Voltage set to: ");
    Serial.print(currentVoltage / 100.0);
    Serial.println("V");
    Serial.print("OVP set to: ");
    Serial.print(ovpVoltage / 100.0);
    Serial.println("V");
    Serial.print("Walk-in time: ");
    Serial.print(walkinTimeLong ? "60" : "5");
    Serial.println(" seconds");
    Serial.println("Settings applied successfully.");
    Serial.println("--------");

    settingsApplied = true;
    if (currentMode == MODE_NORMAL) {
        if (permanentVoltageSet) {
            updateStatusMessage("Settings Applied (Permanent Set)", GREEN);
        } else {
            updateStatusMessage("Settings Applied", GREEN);
        }
    }
}

void processLogInRequest(uint32_t rxID, uint8_t len, uint8_t rxBuf[]) {
    Serial.println("--------");
    Serial.print("Found power supply. Serial: ");
    char output[3];

    if (rxID == 0x05014400) {
        for (int i = 0; i < 6; ++i) {
            serialNumber[i] = rxBuf[i];
            snprintf(output, 3, "%.2X", serialNumber[i]);
            Serial.print(output);
        }
        serialNumber[6] = 0x00;
        serialNumber[7] = 0x00;
    } else if ((rxID & 0xFFFF0000) == 0x05000000) {
        for (int i = 0; i < 6; ++i) {
            serialNumber[i] = rxBuf[i + 1];
            snprintf(output, 3, "%.2X", serialNumber[i]);
            Serial.print(output);
        }
        serialNumber[6] = 0x00;
        serialNumber[7] = 0x00;
    }

    serialNumberReceived = true;
    Serial.println();
    Serial.println("--------");
    
    if (currentMode == MODE_NORMAL) {
        updateStatusMessage("PSU Found - Logging In...", YELLOW);
    }
    logIn();
    delay(100);
    applySettings();
}

void processStatusMessage(uint32_t rxID, uint8_t len, uint8_t rxBuf[]) {
    messagesReceived++;

    if (messagesReceived > 40) {
        messagesReceived = 0;
        logIn();
        delay(50);
        applySettings();  // Re-apply temporary settings to maintain connection
    }

    latestStatus.intakeTemp = rxBuf[0];
    latestStatus.current = 0.1f * (rxBuf[1] | (rxBuf[2] << 8));
    latestStatus.outputVoltage = 0.01f * (rxBuf[3] | (rxBuf[4] << 8));
    latestStatus.inputVoltage = rxBuf[5] | (rxBuf[6] << 8);
    latestStatus.outputTemp = rxBuf[7];
    latestStatus.statusID = rxID;
    latestStatus.hasData = true;

    // Determine status
    latestStatus.hasWarning = false;
    latestStatus.hasAlarm = false;
    
    if (rxID == 0x0501400C) {
        latestStatus.statusText = "Walk-in Error";
        latestStatus.hasAlarm = true;
    } else if (rxID == 0x05014010) {
        latestStatus.statusText = "Walk-in Active";
    } else if (rxID == 0x05014004) {
        latestStatus.statusText = "Normal";
    } else if (rxID == 0x05014008) {
        latestStatus.statusText = "Current Limit";
        latestStatus.hasWarning = true;
    }

    if (latestStatus.hasWarning || latestStatus.hasAlarm) {
        uint8_t txBuf[3] = {0x08, latestStatus.hasWarning ? 0x04 : 0x08, 0x00};
        CAN.sendMsgBuf(0x0501BFFC, 1, 3, txBuf);
    }
}

void processWarningOrAlarmMessage(uint32_t rxID, uint8_t len, uint8_t rxBuf[]) {
    bool isWarning = rxBuf[1] == 0x04;
    Serial.println("--------");
    if (isWarning) {
        Serial.print("Warnings:");
    } else {
        Serial.print("Alarms:");
    }

    uint8_t alarms0 = rxBuf[3];
    uint8_t alarms1 = rxBuf[4];

    for (int i = 0; i < 8; ++i) {
        if (alarms0 & (1 << i)) {
            Serial.print(" ");
            Serial.print(alarms0Strings[i]);
        }
        if (alarms1 & (1 << i)) {
            Serial.print(" ");
            Serial.print(alarms1Strings[i]);
        }
    }
    Serial.println();
    Serial.println("--------");
}

void loop() {
    // Handle encoder input
    handleEncoder();
    
    // Update display every second (only in normal mode)
    if (millis() - lastDisplayUpdate >= DISPLAY_UPDATE_INTERVAL) {
        updateDisplay();
        lastDisplayUpdate = millis();
    }

    // Poll for CAN messages
    if (CAN.checkReceive() == CAN_MSGAVAIL) {
        uint32_t rxID;
        uint8_t len = 0;
        uint8_t rxBuf[8];

        CAN.readMsgBuf((unsigned long *)&rxID, &len, rxBuf);
        rxID &= 0x1FFFFFFF;

        if (!settingsApplied) {
            printMessage(rxID, len, rxBuf);
        }

        if (rxID == 0x05014400 || (rxID & 0xFFFF0000) == 0x05000000) {
            processLogInRequest(rxID, len, rxBuf);
        }
        else if ((rxID & 0xFFFFFF00) == 0x05014000) {
            processStatusMessage(rxID, len, rxBuf);
        }
        else if (rxID == 0x0501BFFC) {
            processWarningOrAlarmMessage(rxID, len, rxBuf);
        }
    }
}