#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SD.h>

// --- LED Definitions ---
#define RED_LED_PIN    12
#define GREEN_LED_PIN  13

// --- OLED Setup ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// --- MFRC522 (RFID) Setup ---
#define RST_PIN       17      // Adjust as needed
#define RFID_CS_PIN    5      // RFID module chip select remains on pin 5
MFRC522 mfrc522(RFID_CS_PIN, RST_PIN);

// --- SD Card Setup ---
// According to your wiring, SD card CS is connected to D0.
#define SD_CS_PIN  0

// --- Button Setup ---
// buttonPins[0]: UP, [1]: DOWN, [2]: SELECT, [3]: BACK, [4]: Secondary BACK/Clear.
const uint8_t buttonPins[5] = {32, 33, 25, 26, 27};

// --- Menu Configuration ---
String menuItems[7] = {
  "Read Card Info",
  "Dump Sectors",
  "Clone Tag",
  "Emulate Tag",
  "Scroll Dump",
  "Advanced Data Capture",
  "Clear Display"
};
const uint8_t menuItemCount = 7;
const uint8_t visibleMenuItems = 6; // ~6 lines for a 64px-high display
int currentMenuIndex = 0;  // Overall menu selection index
int menuStartIndex = 0;    // First menu item index shown on OLED

// --- Global Variables for Cloning & Emulation ---
#define TOTAL_SECTORS      16
#define BLOCKS_PER_SECTOR  4
#define TOTAL_BLOCKS (TOTAL_SECTORS * BLOCKS_PER_SECTOR)
byte cardData[TOTAL_BLOCKS][16];  // To store each block's 16 bytes
String lastUID = "";              // Last successfully read UID

// --- Timeouts ---
#define CARD_WAIT_TIMEOUT 5000  // 5 seconds timeout waiting for a card

// --- Forward Declaration ---
bool checkForBack();

// --- LED Helper Functions ---
void blinkLED(uint8_t ledPin, uint8_t times, uint16_t delayTime) {
  for (uint8_t i = 0; i < times; i++) {
    digitalWrite(ledPin, HIGH);
    delay(delayTime);
    digitalWrite(ledPin, LOW);
    delay(delayTime);
  }
}
void indicateReady() {
  digitalWrite(GREEN_LED_PIN, HIGH);
  digitalWrite(RED_LED_PIN, LOW);
}
void indicateReading() {
  blinkLED(GREEN_LED_PIN, 2, 100);
}
void indicateWriting() {
  blinkLED(RED_LED_PIN, 2, 100);
}
void indicateComplete() {
  blinkLED(GREEN_LED_PIN, 3, 100);
}
void indicateError() {
  blinkLED(RED_LED_PIN, 3, 100);
}

// --- SD Card Helper Functions ---
// Initialize the SD card using SD.begin().
// If initialization fails, prompt the user to manually format the SD card.
bool setupSDCard() {
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("SD card initialization failed!");
    Serial.println("Please format your SD card using your computer and try again.");
    return false;
  }
  Serial.println("SD card initialized successfully.");
  return true;
}

// Log a single line of text to a file (append mode).
void logToSD(const char* filename, String data) {
  File file = SD.open(filename, FILE_WRITE);
  if (file) {
    file.println(data);
    file.close();
  } else {
    Serial.print("Error opening ");
    Serial.println(filename);
  }
}

// Log an array of strings (advanced capture output) to a file.
void logAdvancedDataToSD(String lines[], int count) {
  File file = SD.open("advanced.txt", FILE_WRITE);
  if (file) {
    for (int i = 0; i < count; i++) {
      file.println(lines[i]);
    }
    file.close();
  } else {
    Serial.println("Error opening advanced.txt for logging.");
  }
}

// --- Utility Functions ---
// Wait for a card to be presented within timeoutMillis.
bool waitForCard(uint16_t timeoutMillis) {
  unsigned long startTime = millis();
  while (!mfrc522.PICC_IsNewCardPresent()) {
    if (checkForBack()) return false;
    if (millis() - startTime > timeoutMillis) {
      return false;
    }
    delay(50);
  }
  return true;
}

// Convert UID bytes to a hexadecimal string.
String getUIDAsString() {
  String uidStr = "";
  for (uint8_t i = 0; i < mfrc522.uid.size; i++) {
    uidStr += (mfrc522.uid.uidByte[i] < 0x10 ? "0" : "");
    uidStr += String(mfrc522.uid.uidByte[i], HEX);
  }
  uidStr.toUpperCase();
  return uidStr;
}

// --- Text Scrolling Helper ---
// Displays an array of strings on the OLED in pages (6 lines per page).
void displayScrollableText(String lines[], int lineCount) {
  const int visibleTextLines = 6;
  int scrollIndex = 0;
  while (true) {
    display.clearDisplay();
    display.setTextSize(1);
    for (int i = 0; i < visibleTextLines; i++) {
      int lineIdx = scrollIndex + i;
      if (lineIdx < lineCount) {
        display.setCursor(0, i * 10);
        display.println(lines[lineIdx]);
      }
    }
    // Optional scroll indicator.
    display.setCursor(100, 54);
    display.println("<<");
    display.display();

    if (digitalRead(buttonPins[0]) == LOW) {
      if (scrollIndex > 0) scrollIndex--;
      delay(200);
    }
    else if (digitalRead(buttonPins[1]) == LOW) {
      if (scrollIndex < (lineCount - visibleTextLines)) scrollIndex++;
      delay(200);
    }
    else if (digitalRead(buttonPins[3]) == LOW || digitalRead(buttonPins[4]) == LOW) {
      delay(200);
      break;
    }
    delay(100);
  }
}

// --- Trailer Block Decoding ---
// Decodes the 16-byte trailer block into human‑readable strings.
void decodeTrailer(uint8_t trailer[16], String advLines[], int &lineCount) {
  String line;
  // Decode Key A.
  line = "Trailer Key A: ";
  for (int i = 0; i < 6; i++) {
    if (trailer[i] < 0x10) line += "0";
    line += String(trailer[i], HEX);
    if (i < 5) line += " ";
  }
  advLines[lineCount++] = line;
  
  // Raw Access Bits.
  line = "Access Bits: ";
  for (int i = 6; i < 10; i++) {
    if (trailer[i] < 0x10) line += "0";
    line += String(trailer[i], HEX);
    if (i < 9) line += " ";
  }
  advLines[lineCount++] = line;
  
  // Extract access conditions for blocks 0,1,2 and trailer (block 3).
  for (int i = 0; i < 4; i++) {
    uint8_t c1, c2, c3;
    if (i < 3) {
      c1 = (trailer[6] >> (i + 4)) & 0x01;
      c2 = (trailer[7] >> i) & 0x01;
      c3 = (trailer[8] >> i) & 0x01;
    } else {
      c1 = (trailer[6] >> 7) & 0x01;
      c2 = (trailer[7] >> 3) & 0x01;
      c3 = (trailer[8] >> 3) & 0x01;
    }
    line = "Block " + String(i) + " access: C1=" + String(c1) +
           " C2=" + String(c2) + " C3=" + String(c3);
    advLines[lineCount++] = line;
  }
  
  // Decode Key B.
  line = "Trailer Key B: ";
  for (int i = 10; i < 16; i++) {
    if (trailer[i] < 0x10) line += "0";
    line += String(trailer[i], HEX);
    if (i < 15) line += " ";
  }
  advLines[lineCount++] = line;
}

// --- Function Declarations ---
void initOLED();
void initButtons();
void drawMenu();
void checkMenuInputs();
void runMenuItem(uint8_t idx);

void readUID();             // Displays basic UID and card type.
void dumpSectors();         // Dumps all sectors to the Serial Monitor.
void cloneCard();           // Clones card data from a source card to a target card.
void emulateTag();          // Simulated tag emulation (shows stored UID).
void scrollDumpSectors();   // Scrollable display of sectors on the OLED.
void advancedDataCapture(); // Detailed card info capture and trailer decoding.
void clearDisplayMode();
bool checkForBack();        // Checks if a BACK button is pressed.

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);  // Initialize I2C for the OLED.
  initOLED();
  initButtons();

  // Initialize LED pins.
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);
  indicateReady();

  // Initialize SPI first.
  SPI.begin();
  
  // Now set up the SD card.
  if (!setupSDCard()) {
    Serial.println("SD card setup failed. SD logging will be disabled.");
  }

  mfrc522.PCD_Init();

  drawMenu();
}

// ===== LOOP =====
void loop() {
  checkMenuInputs();
  delay(100);
}

// ===== OLED INITIALIZATION =====
void initOLED() {
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("SSD1306 allocation failed");
    while (1);
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.display();
}

// ===== DRAW MENU (Scrolling Window) =====
void drawMenu() {
  display.clearDisplay();
  if (currentMenuIndex < menuStartIndex)
    menuStartIndex = currentMenuIndex;
  if (currentMenuIndex >= menuStartIndex + visibleMenuItems)
    menuStartIndex = currentMenuIndex - visibleMenuItems + 1;

  for (uint8_t i = 0; i < visibleMenuItems; i++) {
    uint8_t idx = menuStartIndex + i;
    if (idx >= menuItemCount) break;
    if (idx == currentMenuIndex)
      display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    else
      display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
    display.setCursor(0, i * 10);
    display.println(menuItems[idx]);
  }
  display.display();
}

// ===== INITIALIZE BUTTONS =====
void initButtons() {
  for (uint8_t i = 0; i < 5; i++) {
    pinMode(buttonPins[i], INPUT_PULLUP);
  }
}

// ===== MENU NAVIGATION =====
void checkMenuInputs() {
  if (digitalRead(buttonPins[0]) == LOW) {  // UP
    delay(200);
    currentMenuIndex = (currentMenuIndex == 0) ? menuItemCount - 1 : currentMenuIndex - 1;
    drawMenu();
  }
  if (digitalRead(buttonPins[1]) == LOW) {  // DOWN
    delay(200);
    currentMenuIndex = (currentMenuIndex + 1) % menuItemCount;
    drawMenu();
  }
  if (digitalRead(buttonPins[2]) == LOW) {  // SELECT
    delay(200);
    runMenuItem(currentMenuIndex);
    drawMenu();  // Redraw menu after operation.
  }
}

// ===== EXECUTE SELECTED MENU ITEM =====
void runMenuItem(uint8_t idx) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
  display.setCursor(0, 0);
  display.println("Selected:");
  display.println(menuItems[idx]);
  display.display();
  delay(500);

  switch (idx) {
    case 0: readUID(); break;
    case 1: dumpSectors(); break;
    case 2: cloneCard(); break;
    case 3: emulateTag(); break;
    case 4: scrollDumpSectors(); break;
    case 5: advancedDataCapture(); break;
    case 6: clearDisplayMode(); break;
    default: break;
  }
  delay(500);
  indicateReady();
}

// ===== READ UID =====
void readUID() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Place card for info");
  display.display();

  if (!waitForCard(CARD_WAIT_TIMEOUT)) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("No Card Detected");
    display.display();
    indicateError();
    delay(1500);
    return;
  }

  indicateReading();
  if (!mfrc522.PICC_ReadCardSerial()) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Read Failed.");
    display.display();
    indicateError();
    delay(1000);
    return;
  }

  String uidStr = getUIDAsString();
  lastUID = uidStr;
  MFRC522::PICC_Type piccType = mfrc522.PICC_GetType(mfrc522.uid.sak);
  String cardType = String(mfrc522.PICC_GetTypeName(piccType));

  Serial.println("UID: " + uidStr);
  Serial.println("Type: " + cardType);

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("UID:");
  display.println(uidStr);
  display.println("Type:");
  display.println(cardType);
  display.display();
  delay(2000);

  // Log basic read to SD card.
  logToSD("read.txt", "UID: " + uidStr + "  Type: " + cardType);

  indicateComplete();
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}

// ===== DUMP SECTORS =====
void dumpSectors() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Dumping sectors...");
  display.display();

  MFRC522::StatusCode status;
  MFRC522::MIFARE_Key key;
  for (uint8_t i = 0; i < 6; i++) key.keyByte[i] = 0xFF;

  for (uint8_t sector = 0; sector < TOTAL_SECTORS; sector++) {
    for (uint8_t block = 0; block < BLOCKS_PER_SECTOR; block++) {
      uint8_t blockAddr = sector * BLOCKS_PER_SECTOR + block;
      uint8_t buffer[18];
      uint8_t size = sizeof(buffer);

      status = mfrc522.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, blockAddr, &key, &(mfrc522.uid));
      if (status != MFRC522::STATUS_OK) {
        Serial.print("Auth failed for block ");
        Serial.println(blockAddr);
        continue;
      }
      status = mfrc522.MIFARE_Read(blockAddr, buffer, &size);
      if (status != MFRC522::STATUS_OK) {
        Serial.print("Read failed for block ");
        Serial.println(blockAddr);
        continue;
      }
      Serial.print("Sector ");
      Serial.print(sector);
      Serial.print(", Block ");
      Serial.print(block);
      Serial.print(": ");
      for (uint8_t i = 0; i < 16; i++) {
        if (buffer[i] < 0x10) Serial.print("0");
        Serial.print(buffer[i], HEX);
        Serial.print(" ");
      }
      Serial.println();
    }
  }
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Dump Complete");
  display.println("See Serial Monitor");
  display.display();
  delay(1500);
}

// ===== CLONE CARD =====
// Skips sector 0 (manufacturer data is read‑only).
void cloneCard() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Place SOURCE card");
  display.display();

  if (!waitForCard(CARD_WAIT_TIMEOUT)) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("No Card Detected");
    display.display();
    indicateError();
    delay(1500);
    return;
  }

  indicateReading();
  if (!mfrc522.PICC_ReadCardSerial()) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Source read failed");
    display.display();
    indicateError();
    delay(1000);
    return;
  }

  MFRC522::StatusCode status;
  MFRC522::MIFARE_Key key;
  for (uint8_t i = 0; i < 6; i++) key.keyByte[i] = 0xFF;

  // Read and store data from sectors 1 and up.
  for (uint8_t sector = 1; sector < TOTAL_SECTORS; sector++) {
    for (uint8_t block = 0; block < BLOCKS_PER_SECTOR; block++) {
      uint8_t blockAddr = sector * BLOCKS_PER_SECTOR + block;
      uint8_t buffer[18];
      uint8_t size = sizeof(buffer);
      status = mfrc522.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, blockAddr, &key, &(mfrc522.uid));
      if (status != MFRC522::STATUS_OK) {
        Serial.print("Auth failed for block ");
        Serial.println(blockAddr);
        continue;
      }
      status = mfrc522.MIFARE_Read(blockAddr, buffer, &size);
      if (status != MFRC522::STATUS_OK) {
        Serial.print("Read failed for block ");
        Serial.println(blockAddr);
        continue;
      }
      memcpy(cardData[blockAddr], buffer, 16);
    }
  }
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Source OK");
  display.println("Remove source");
  display.println("Place TARGET card");
  display.display();
  delay(1000);

  if (!waitForCard(CARD_WAIT_TIMEOUT)) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("No Target Card");
    display.display();
    indicateError();
    delay(1500);
    return;
  }

  indicateWriting();
  if (!mfrc522.PICC_ReadCardSerial()) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Target read failed");
    display.display();
    indicateError();
    delay(1000);
    return;
  }

  // Write data to target card, skipping sector 0.
  for (uint8_t sector = 1; sector < TOTAL_SECTORS; sector++) {
    for (uint8_t block = 0; block < BLOCKS_PER_SECTOR; block++) {
      uint8_t blockAddr = sector * BLOCKS_PER_SECTOR + block;
      uint8_t buffer[18];
      memcpy(buffer, cardData[blockAddr], 16);
      status = mfrc522.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, blockAddr, &key, &(mfrc522.uid));
      if (status != MFRC522::STATUS_OK) {
        Serial.print("Auth failed (write) for block ");
        Serial.println(blockAddr);
        continue;
      }
      status = mfrc522.MIFARE_Write(blockAddr, buffer, 16);
      if (status != MFRC522::STATUS_OK) {
        Serial.print("Write failed for block ");
        Serial.println(blockAddr);
        continue;
      }
    }
  }
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Cloning complete");
  display.println("Check target card");
  display.display();
  indicateComplete();
  delay(1500);
}

// ===== EMULATE TAG =====
void emulateTag() {
  display.clearDisplay();
  display.setCursor(0, 0);

  if (lastUID == "") {
    display.println("No UID stored");
    display.println("Read a card first");
    display.display();
    indicateError();
    delay(1500);
    return;
  }

  display.println("Entering Emulation");
  display.println("Press BACK to exit");
  display.display();
  delay(1000);

  while (true) {
    if (checkForBack()) break;
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Emulated Card:");
    display.println(lastUID);
    display.println();
    display.println("MFRC522 cannot");
    display.println("truly emulate RFID");
    display.display();
    delay(500);
  }
}

// ===== SCROLL DUMP SECTORS =====
void scrollDumpSectors() {
  MFRC522::StatusCode status;
  MFRC522::MIFARE_Key key;
  for (uint8_t i = 0; i < 6; i++) key.keyByte[i] = 0xFF;

  for (uint8_t sector = 0; sector < TOTAL_SECTORS; sector++) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.print("Sector ");
    display.println(sector);

    for (uint8_t block = 0; block < BLOCKS_PER_SECTOR; block++) {
      uint8_t blockAddr = sector * BLOCKS_PER_SECTOR + block;
      uint8_t buffer[18];
      uint8_t size = sizeof(buffer);

      status = mfrc522.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, blockAddr, &key, &(mfrc522.uid));
      if (status != MFRC522::STATUS_OK) {
        display.print("B"); display.print(block);
        display.println(": Auth Err");
        continue;
      }
      status = mfrc522.MIFARE_Read(blockAddr, buffer, &size);
      if (status != MFRC522::STATUS_OK) {
        display.print("B"); display.print(block);
        display.println(": Read Err");
        continue;
      }
      String hexStr = "";
      for (uint8_t i = 0; i < 16; i++) {
        if (buffer[i] < 0x10) hexStr += "0";
        hexStr += String(buffer[i], HEX);
        hexStr += " ";
      }
      display.print("B"); display.print(block);
      display.print(": ");
      display.println(hexStr);
    }
    display.println();
    display.println("SEL:Next, BACK:Exit");
    display.display();

    while (true) {
      if (digitalRead(buttonPins[2]) == LOW) { delay(200); break; }
      if (checkForBack()) return;
      delay(50);
    }
  }
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Scroll Dump done");
  display.display();
  delay(1500);
}

// ===== ADVANCED DATA CAPTURE =====
// Captures detailed card info (UID, type, sector data) and decodes trailer blocks.
// Output is collected into an array and scrolled on the OLED.
// Additionally, the output is logged to the SD card.
void advancedDataCapture() {
  String advancedLines[256];
  int advancedLineCount = 0;

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Advanced Capture");
  display.println("Place card...");
  display.display();

  if (!waitForCard(CARD_WAIT_TIMEOUT)) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("No Card Detected");
    display.display();
    indicateError();
    delay(1500);
    return;
  }

  indicateReading();

  if (!mfrc522.PICC_ReadCardSerial()) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Card read failed");
    display.display();
    indicateError();
    delay(1000);
    return;
  }

  String uidStr = getUIDAsString();
  lastUID = uidStr;
  MFRC522::PICC_Type piccType = mfrc522.PICC_GetType(mfrc522.uid.sak);
  String cardType = String(mfrc522.PICC_GetTypeName(piccType));

  Serial.println("=== Advanced Data Capture ===");
  Serial.println("UID: " + uidStr);
  Serial.println("Card Type: " + cardType);

  advancedLines[advancedLineCount++] = "=== Advanced Capture ===";
  advancedLines[advancedLineCount++] = "UID: " + uidStr;
  advancedLines[advancedLineCount++] = "Type: " + cardType;

  MFRC522::MIFARE_Key key;
  for (uint8_t i = 0; i < 6; i++) key.keyByte[i] = 0xFF;

  for (uint8_t sector = 0; sector < TOTAL_SECTORS; sector++) {
    String sectorLine = "Sector " + String(sector);
    Serial.println(sectorLine);
    advancedLines[advancedLineCount++] = sectorLine;

    for (uint8_t block = 0; block < BLOCKS_PER_SECTOR; block++) {
      uint8_t blockAddr = sector * BLOCKS_PER_SECTOR + block;
      uint8_t buffer[18];
      uint8_t size = sizeof(buffer);

      MFRC522::StatusCode status = mfrc522.PCD_Authenticate(
          MFRC522::PICC_CMD_MF_AUTH_KEY_A, blockAddr, &key, &(mfrc522.uid));
      if (status != MFRC522::STATUS_OK) {
        String err = "  Block " + String(block) + ": Auth Failed";
        Serial.println(err);
        advancedLines[advancedLineCount++] = err;
        continue;
      }
      status = mfrc522.MIFARE_Read(blockAddr, buffer, &size);
      if (status != MFRC522::STATUS_OK) {
        String err = "  Block " + String(block) + ": Read Failed";
        Serial.println(err);
        advancedLines[advancedLineCount++] = err;
        continue;
      }
      String blockData = "  Block " + String(block) + ": ";
      for (uint8_t i = 0; i < 16; i++) {
        if (buffer[i] < 0x10) blockData += "0";
        blockData += String(buffer[i], HEX) + " ";
      }
      Serial.println(blockData);
      advancedLines[advancedLineCount++] = blockData;

      if (block == BLOCKS_PER_SECTOR - 1) {
        decodeTrailer(buffer, advancedLines, advancedLineCount);
      }
    }
  }

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Capture Complete!");
  display.display();
  indicateComplete();
  delay(1000);

  // Log advanced capture output to SD card.
  logAdvancedDataToSD(advancedLines, advancedLineCount);

  // Allow user to scroll through advanced capture output.
  displayScrollableText(advancedLines, advancedLineCount);
}

// ===== CLEAR DISPLAY MODE =====
void clearDisplayMode() {
  display.clearDisplay();
  display.display();
  delay(500);
}

// ===== CHECK FOR BACK BUTTON PRESS =====
bool checkForBack() {
  if (digitalRead(buttonPins[3]) == LOW || digitalRead(buttonPins[4]) == LOW) {
    delay(200);
    return true;
  }
  return false;
}
