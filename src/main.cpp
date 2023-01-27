// *******************************************************************************************************
// DESCRIPTION: Garage control program v1.0
// AUTOR: Fran Guillén
// BOARD: Wemos D1 mini
// *****************************************************************************************************

#define DEBUGGING

#include <debug.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>         // MQTT library
#include <ArduinoJson.h>          // JSON library
#include <time.h>
#include <EEPROM.h>

// ---------- SENSITIVE INFORMATION --------
#include <secrets.h>
#include <CertificateFile.h>



// ---------- DEFAULT SYSTEM CONFIGURATION ---------
#define SENSOR_READ_INTERVAL  500


// ---------- ANALOG INPUTS ----------
#define AI_POT  A0


// ---------- DIGITAL OUTPUTS ----------
#define DO_LED         2

// --------- EEPROM ---------
#define EEPROM_SIZE   10

// ---------- WIFI AND MQTT CONNECTION (secrets.h) ----------
#define WIFI_SSID       SECRET_WIFI_SSID               // WIFI SSID
#define WIFI_PASSWORD   SECRET_WIFI_PASSWORD           // WIFI password
#define MQTT_SERVER     SECRET_MQTT_SERVER             // MQTT server IP
#define MQTT_PORT       SECRET_MQTT_PORT               // MQTT port
#define MQTT_ID         SECRET_MQTT_ID                 // MQTT id
#define MQTT_USERNAME   SECRET_MQTT_USERNAME           // MQTT server username
#define MQTT_PASSWORD   SECRET_MQTT_PASSWORD           // MQTT server password

// ---------- MQTT TOPICS ----------
#define TOPIC_PUB_DIESEL_LEVEL          "anasanchez/diesel/level"
#define TOPIC_SUB                       "anasanchez/diesel/device/config/#"
#define TOPIC_SUB_CONFIG_CALIBRATION    "anasanchez/diesel/device/config/calibration"
#define TOPIC_SUB_CONFIG                "anasanchez/diesel/device/config"
#define TOPIC_PUB_DEV_STAT              "anasanchez/diesel/device/info"

// --------- WILL MESSAGE ---------
#define WILL_MESSAGE  R"EOF("{"status": "disconnected"}")EOF"


// ********************************************************************
//                     FUNCTION PROTOTYPES
// ********************************************************************
void SendDeviceStatus();
void ConnectWifi();
bool ConnectMQTT();
unsigned long UnixTime();
void UpdateSensorLevel();
void RelayControl(bool);
char* GetDeviceId();
void OnReceiveMQTT(char *, byte *, unsigned int);


// ********************************************************************
//                     GLOBAL VARIABLES
// ********************************************************************
// ---------- WIFI AND MQTT CLIENT ----------
//X509List caCertX509(caCert);
WiFiManager wifiManager;
//WiFiClientSecure  espClient;
WiFiClient  espClient;
PubSubClient mqttClient(espClient);

// ---------- SENSOR CALIBRATION ----------
uint16_t sensor_calibration_full;
uint16_t sensor_calibration_empty;
const int address_memory_sensor_calibration_empty = 0;
const int address_memory_sensor_calibration_full = 2;




// ********************************************************************
//                     BOARD SETUP
// ********************************************************************
void setup() {
  
  // --------- DEBUG ---------
  wifiManager.setDebugOutput(false);
  #ifdef DEBUGGING
    Serial.begin(9600);
    wifiManager.setDebugOutput(true);
  #endif

  // ---------- PIN CONFIG ----------
  pinMode(DO_LED, OUTPUT);
  digitalWrite(DO_LED, HIGH);

      // --------- WIFI MANAGER CONFIG --------
  WiFi.mode(WIFI_STA); // explicitly set mode, esp defaults to STA+AP
  wifiManager.setClass("invert");
  wifiManager.autoConnect("WiFi Diesel");


  // --------- EEPROM CONFIG (get sensor calibration values) ---------

  // EEPROM.begin(EEPROM_SIZE);
  // EEPROM.put(0, sensor_calibration_empty);
  // EEPROM.put(2, sensor_calibration_full);
  // EEPROM.commit();
  // EEPROM.end();

  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(address_memory_sensor_calibration_empty, sensor_calibration_empty);
  EEPROM.get(address_memory_sensor_calibration_full, sensor_calibration_full);
  EEPROM.end();

  DEBUG("Sensor calibration empty: ");
  DEBUGLN(sensor_calibration_empty);
  DEBUG("Sensor calibration full: ");
  DEBUGLN(sensor_calibration_full);


  

  // --------- NTP SERVER CONFIG ---------
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  DEBUG("Getting time ...");
  time_t now2 = time(nullptr);
  while (now2 < 1635448489) {
    delay(500);
    DEBUG(".");
    now2 = time(nullptr);
  }
  DEBUGLN();
  DEBUG("Time: ");
  DEBUGLN(now2);

  // --------- CONFIG CA CERT --------
  // Necessary to get correct time
    //espClient.setTrustAnchors(&caCertX509);
    //espClient.setInsecure();

  
  // ---------- MQTT CONNECTION --------
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(OnReceiveMQTT);
}


// ********************************************************************
//                      CALLBACKS
// ********************************************************************
void OnReceiveMQTT(char *topic, byte *payload, unsigned int length) {
  DEBUG("Message arrived [");
  DEBUG(topic);
  DEBUG("] ");
  for (unsigned int i = 0; i < length; i++) {
    DEBUG((char)payload[i]);
  }
  DEBUGLN();

  // --------- MQTT VARIABLES ----------
  StaticJsonDocument<100> doc;
  deserializeJson(doc, payload, length);
  JsonObject obj = doc.as<JsonObject>();

  // ---------- FILTERING BY TOPIC ----------
  if (strcmp(topic, TOPIC_SUB_CONFIG_CALIBRATION) == 0) {
    if (obj.containsKey("calibrationEmpty")) {
      sensor_calibration_empty = obj["calibrationEmpty"];
    }

    if (obj.containsKey("calibrationFull")) {
      sensor_calibration_full = obj["calibrationFull"];
    }

    else if(obj.containsKey("automaticCalibration")) {
      const char* level = obj["automaticCalibration"].as<const char*>();
      if(level[0] == 'f' || level[0] == 'F') {
        sensor_calibration_full = analogRead(AI_POT);
      }
      else if(level[0] == 'e' || level[0] == 'E') {
        sensor_calibration_empty = analogRead(AI_POT);
      }
    }

    EEPROM.begin(EEPROM_SIZE);
    EEPROM.put(address_memory_sensor_calibration_empty, sensor_calibration_empty);
    EEPROM.put(address_memory_sensor_calibration_full, sensor_calibration_full);
    EEPROM.end();

  }

  else if(strcmp(topic, TOPIC_SUB_CONFIG ) == 0) {
    // ---------- CHANGING SAMPLE TIME ---------
    if (obj.containsKey("deviceStatus")) {

    }
  }
  SendDeviceStatus();
  UpdateSensorLevel();
}

// ********************************************************************
//                           LOOP
// ********************************************************************
void loop() {

  // ---------- READING AND SENDING DATA ---------
  static unsigned long prev_millis_lecture = 0;
  if(millis() - prev_millis_lecture >= SENSOR_READ_INTERVAL) {
    prev_millis_lecture = millis();
    UpdateSensorLevel();
  }


  // ---------- MANAGE MQTT CONNECTION (NON BLOCKING) ----------
  static unsigned long lastReconnectAttempt = 0;
  if (!mqttClient.connected() && time(nullptr) > 1635448489) {
    if (millis() - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = millis();
      if (ConnectMQTT()) {
        DEBUGLN("MQTT connected");
        lastReconnectAttempt = 0;
        UpdateSensorLevel();
      }
      else {
        DEBUGLN("MQTT connection error");
      }
    }
  }

  // ---------- CONNECTION PROCESS ----------
  mqttClient.loop();
}



// ********************************************************************
//                      LOCAL FUNCTIONS
// ********************************************************************

void SendDeviceStatus() {
  const int capacity = 200;
  StaticJsonDocument<capacity> doc;
  char buffer[230];

  doc["status"] = "connected";
  doc["id(mac)"] = GetDeviceId();
  doc["heapFragmentation"] = ESP.getHeapFragmentation();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["IP"] = WiFi.localIP();
  
  JsonArray data = doc.createNestedArray("sensorCalibration");
  data.add(sensor_calibration_empty);
  data.add(sensor_calibration_full);

  serializeJsonPretty(doc, buffer);
  mqttClient.publish(TOPIC_PUB_DEV_STAT, buffer, true);
}
  

bool ConnectMQTT() {
  // Attempt to connect
  if (mqttClient.connect(MQTT_ID, MQTT_USERNAME, MQTT_PASSWORD, TOPIC_PUB_DEV_STAT, 1, true, WILL_MESSAGE)) {
    SendDeviceStatus();
    mqttClient.subscribe(TOPIC_SUB, 1);
    }

  return mqttClient.connected();
}


// --------- UPDATE SENSOR STATUS AND SEND MQTT SERVER --------
void UpdateSensorLevel() {
  const int capacity = JSON_OBJECT_SIZE(3);
  StaticJsonDocument<capacity> doc;
  char buffer[80];

  int adc_pot_read = analogRead(AI_POT);

  doc["level_adc"] = adc_pot_read;
  doc["level_percentage"] = map(adc_pot_read, sensor_calibration_empty, sensor_calibration_full, 0, 100);
  doc["ts"] = (int)time(nullptr);

  serializeJsonPretty(doc, buffer);
  mqttClient.publish(TOPIC_PUB_DIESEL_LEVEL, buffer, true); 
}

char* GetDeviceId() {
  uint8_t mac[6];
  wifi_get_macaddr(STATION_IF, mac);
  char *string = (char*)malloc(13);
  sprintf(string, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return string;
}