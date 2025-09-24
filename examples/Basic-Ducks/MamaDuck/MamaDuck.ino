/**
 * @file MamaDuck.ino
 * @brief Uses the built in Mama Duck with a BMP390 integrated.
 */

#include <string>
#include <vector>
#include <arduino-timer.h>
#include <CDP.h>

#ifdef SERIAL_PORT_USBVIRTUAL
#define Serial SERIAL_PORT_USBVIRTUAL
#endif

#include <Wire.h>

#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>
XPowersPMU PMU;

bool sendData(std::string message, byte topic);
bool runHealthSensor(void *);
std::string getBatteryData();
MamaDuck duck;

const int INTERVAL_HEALTH = 1000*60;     // Frequency of status messages
std::string deviceId("MAMADESK");  // MUST BE 8 bytes (characters)

// create a timer with default settings
auto timer = timer_create_default();

int counter = 1;
bool setupOK = false;
int lastFreeMemory = 0;

void setup() {

  std::array<byte,8> devId;
  std::copy(deviceId.begin(), deviceId.end(), devId.begin());
  if (duck.setupWithDefaults(devId) != DUCK_ERR_NONE) {
    Serial.println("[MAMA] Failed to setup MamaDuck");
    return;
  }

  Wire.begin(21, 22);
     if (!PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, 21, 22)) {
     Serial.println("[MAMA] AXP2101 Begin FAIL");
     return;
   } else {
     Serial.println("[MAMA] AXP2101 Begin PASS");
     PMU.enableBattDetection();
     PMU.enableVbusVoltageMeasure();
     PMU.enableBattVoltageMeasure();
     PMU.enableSystemVoltageMeasure();
     PMU.enableTemperatureMeasure();
   }

  timer.every(INTERVAL_HEALTH, runHealthSensor);

  // // Setup Complete
  Serial.println("[MAMA] Setup OK!");
  setupOK = true;
}

void loop() {
  
  if (!setupOK) {
    return; 
  }

  timer.tick();
  
  duck.run();
}

bool runHealthSensor(void *) {
  
  bool result;

  int currentFreeMemory = freeMemory();
  std::string message = "{\"C\":" + std::to_string(counter) + ",\"DFM\":" + std::to_string(currentFreeMemory-lastFreeMemory) + "," + getBatteryData() + "}";
  lastFreeMemory = currentFreeMemory;
  
  Serial.println(message.c_str());

  result = sendData(message, topics::health);
  if (result) {
     Serial.println("[MAMA] runSensor ok.");
     counter++;
  } else {
     Serial.println("[MAMA] runSensor failed.");
  }
  return result;
}

bool sendData(std::string message, byte topic) {
  bool sentOk = false;
  
  int err = duck.sendData(topic, message);
  if (err == DUCK_ERR_NONE) {
     sentOk = true;
  }
  if (!sentOk) {
    Serial.println((std::string("[MAMA] Failed to send data. error = ") + std::to_string(err)).c_str());
  }
  return sentOk;
}

// bool runBMPSensor(void *) {

//   bool result;

//   std::string bmpData;

//   Serial.println("[MAMA] --- BMP ---");
//   if (! bmp.performReading()) {
//     Serial.println("[MAMA] Failed to perform reading :(");
//     bmpData = "{T:No Reading,P:No Reading,A:No Reading}";
//   }

//   float T = bmp.temperature;
//   Serial.print("[MAMA] Temperature: "); Serial.println(T);
//   float P = bmp.pressure;
//   Serial.print("[MAMA] Pressure: "); Serial.println(P);
//   float A = bmp.readAltitude(SEALEVELPRESSURE_HPA);
//   Serial.print("[MAMA] Altitude: "); Serial.println(A);

//   bmpData = "{T:" + std::to_string(T) + ",P:" + std::to_string(P) + ",A:" + std::to_string(A) + "}";

//   result = sendData(bmpData, topics::bmp180);
//   if (result) {
//      Serial.println("[MAMA] runBMPSensor ok.");
//   } else {
//      Serial.println("[MAMA] runSensor failed.");
//   }

//   return result;
// }

std::string getBatteryData() {
   
  // Check if battery is connected
  if (!PMU.isBatteryConnect()) {
    Serial.println("[DUCK] No battery connected");
    return std::string("No battery");
  }
   
  // Get battery data from AXP2101 using proper XPowersPMU methods
  float voltage = PMU.getBattVoltage() / 1000.0;  // Convert mV to V
  float percentage = PMU.getBatteryPercent();     // Now works with XPowersPMU
  bool charging = PMU.getVbusVoltage() > 4000;    // Charging if VBUS > 4V
   
  // Get temperature from AXP2101 (XPowersPMU returns correct scale)
  float temperature = PMU.getTemperature();
   
  // std::string healthMessage = "\"V\":" + std::to_string(voltage) + "," +
  //                              "\"PCT\":" + std::to_string(percentage) + "," +
  //                              "\"CHRG\":" + (charging ? "true" : "false") + "," +
  //                              "\"BT\":" + std::to_string(temperature);

  std::string s;
  s.reserve(96);
  s += "\"V\":";    s += std::to_string(voltage);
  s += ",\"PCT\":"; s += std::to_string(percentage);
  s += ",\"CHRG\":"; s += (charging ? "true" : "false");  // <-- boolean literal
  s += ",\"BT\":";  s += std::to_string(temperature);
  
  Serial.println(("[DUCK] Health data: " + s).c_str());

  return s;
 }

// // Getting GPS data
// bool runGPSSensor(void *) {

//   bool result;
//   // Encoding the GPS
//   smartDelay(5000);
  
//   // Printing the GPS data
//   Serial.println("[MAMA] --- GPS ---");
//   Serial.print("[MAMA] Latitude  : ");
//   Serial.println(tgps.location.lat(), 5);  
//   Serial.print("[MAMA] Longitude : ");
//   Serial.println(tgps.location.lng(), 4);
//   Serial.print("[MAMA] Altitude  : ");
//   Serial.print(tgps.altitude.feet() / 3.2808);
//   Serial.println("M");
//   Serial.print("[MAMA] Satellites: ");
//   Serial.println(tgps.satellites.value());
//   Serial.print("[MAMA] Time      : ");
//   Serial.print(tgps.time.hour());
//   Serial.print(":");
//   Serial.print(tgps.time.minute());
//   Serial.print(":");
//   Serial.println(tgps.time.second());
//   Serial.print("[MAMA] Speed     : ");
//   Serial.println(tgps.speed.kmph());
//   Serial.println("[MAMA] **********************");
  
//   // Creating a message of the Latitude and Longitude
//   std::string gpsData = "{Lat:" + std::to_string(tgps.location.lat()) + ",Lng:" + std::to_string(tgps.location.lng()) + ",Alt:" + std::to_string(tgps.altitude.feet() / 3.2808) + "}";

//   result = sendData(gpsData, topics::gps);
//   if (result) {
//      Serial.println("[MAMA] runGPSSensor ok.");
//   } else {
//      Serial.println("[MAMA] runGPSSensor failed.");
//   }

//   return result;
// }