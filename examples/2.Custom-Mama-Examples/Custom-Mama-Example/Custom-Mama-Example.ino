/**
 * @file Custom-Mama-Example.ino
 * 
 * @brief Builds a MamaDuck using APIs from CDP.
 * 
 * This example is a Custom Mama Duck with an OLED display, and it is
 * periodically sending a message in the Mesh
 * 
 */

#include <string>
#include <arduino-timer.h>
#include <CDP.h>

#ifdef SERIAL_PORT_USBVIRTUAL
#define Serial SERIAL_PORT_USBVIRTUAL
#endif

std::string deviceId("MAMA0001");   // The deviceId must be 8 bytes (characters)
const int INTERVAL_MS = 60000;      // Interval for runSensor function

bool sendData(std::vector<byte> message, topics value);
bool runSensor(void *);
std::vector<byte> stringToByteVector(const String& str);

// create a built-in mama duck
MamaDuck duck;

// Create a Display Instance
DuckDisplay* display = NULL;

// LORA RF CONFIG
#define LORA_FREQ 915.0 // Frequency Range. Set for US Region 915.0Mhz
#define LORA_TXPOWER 20 // Transmit Power
// LORA HELTEC PIN CONFIG
#define LORA_CS_PIN 18
#define LORA_DIO0_PIN 26
#define LORA_DIO1_PIN -1 // unused
#define LORA_RST_PIN 14

// create a timer with default settings
auto timer = timer_create_default();

int counter = 1;

void setup() {
  
  std::vector<byte> devId;
  devId.insert(devId.end(), deviceId.begin(), deviceId.end()); 

  duck.setDeviceId(devId);    // set the Device ID in the Duck
  
  duck.setupSerial(115200);   // initialize the serial component with the hardware supported baudrate
  
  duck.setupRadio(            // initialize the LoRa radio with specific settings. This overwrites settings in cdpcfg.h
    LORA_FREQ, 
    LORA_CS_PIN, 
    LORA_RST_PIN, 
    LORA_DIO0_PIN, 
    LORA_DIO1_PIN, 
    LORA_TXPOWER
  );
  
  duck.setupWifi();            // initialize the wifi access point with a custom AP name

  duck.setupDns();             // initialize DNS
  
  duck.setupWebServer(true);   // initialize web server, enabling the captive portal with a custom HTML page

  duck.setupOTA();             // initialize Over The Air firmware upgrade
  
  // Get an instance of DuckLED and initialize it
  display = DuckDisplay::getInstance();
  display->setupDisplay(duck.getType(), devId);
  display->showDefaultScreen();

  // Initialize the timer to trigger sending a counter message.
  timer.every(INTERVAL_MS, runSensor);
  
  Serial.println("[MAMA] Setup OK!");

}

void loop() {
  timer.tick();
  // Use the default run(). The Mama duck is designed to also forward data it receives
  // from other ducks, across the network. It has a basic routing mechanism built-in
  // to prevent messages from hoping endlessly.
  duck.run();
}

std::vector<byte> stringToByteVector(const String& str) {
    std::vector<byte> byteVec;
    byteVec.reserve(str.length());

    for (unsigned int i = 0; i < str.length(); ++i) {
        byteVec.push_back(static_cast<byte>(str[i]));
    }

    return byteVec;
}

bool runSensor(void *) {
  
  bool result;
  
  String message = String("Counter:") + String(counter)+ " " +String("Free Memory:") + String(freeMemory());
  Serial.print("[MAMA] status data: ");
  Serial.println(message);

  result = sendData(stringToByteVector(message), topics::status);
  
  if (result) {
     Serial.println("[MAMA] runSensor ok.");
  } else {
     Serial.println("[MAMA] runSensor failed.");
  }
  
  return result;

}

bool sendData(std::vector<byte> message, topics value) {
  bool sentOk = false;
  
  int err = duck.sendData(value, message);
  if (err == DUCK_ERR_NONE) {
     counter++;
     sentOk = true;
  }
  if (!sentOk) {
    Serial.println("[MAMA] Failed to send data. error = " + String(err));
  }
  return sentOk;
}