//
// ANDRIS Pool Thermometer APT Firmware
// Wifi floating water sensor array.
//
#include "Esp.h"
#include <FS.h>
#include <string.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <WiFiClientSecureBearSSL.h>
#include <ESP8266WiFiType.h>
#include <ESP8266WiFiGeneric.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <Ticker.h>

#define FIRMWARE_URL "/getFirmwareDownloadUrl"
#define API_SERVER_URL "us-central1-chas-c2689.cloudfunctions.net" //TODO: move to config file
#define ONBOARD_API_ENDPOINT "/onboardDevice"
#define USER_FOR_DEVICE_API_ENDPOINT "/userForDevice"
#define SENSOR_DATA_API_ENDPOINT "/sensorData"
#define DEVICE_STATUS_API_ENDPOINT "/deviceStatus"
#define DEBUG_API_ENDPOINT "/consoleLog"

#define CONFIG_FILENAME "/config.json"
#define ONBOARD_SSID "Galactica"
#define ONBOARD_PASSWORD "archiefifi"

#define AP_NAME "Pool Thermometer"
#define WATER_TEMP_SENSOR "water_temp"
#define PUBLISH_DEVICE_CYCLES 3 // number of restarts before publishing device status

#define DEBUG Serial
//#define DEBUG_MODE
#define DEBUG_UPDATER Serial
//#define TOTAL_RESET

OneWire oneWire(TEMP_SENSOR_PIN);
DallasTemperature dallas_sensors(&oneWire);
Ticker ticker;

// System
char device_chipId[13];
int rebootsSinceLastDevicePost = 0;
String device_id;
String user_id;

struct APIResponse
{
  int code;
  String payload;
};

void setChipString()
{
  uint64_t chipid;
  chipid = ESP.getChipId();
  uint16_t chip = (uint16_t)(chipid >> 32);
  snprintf(device_chipId, 13, "%04X%08X", chip, (uint32_t)chipid);
}

void onboard();
void loadConfig();
void provision();
int wifiStrengthInBars();
void tick();
void connectToWifi(String ssid, String password);
APIResponse HttpJSONStringToEndPoint(String JSONString, const char *endPoint);
void publishTemperature();
void device_Maintance();
void publishDeviceStatus();
String getDownloadUrl();
bool downloadUpdate(String url);
void debug(String string, bool publish = false);
void apConfigSaveCallback();
void ota_update();

void setup()
{
  pinMode(STATUS_LED, OUTPUT);
  Serial.begin(115200);
  delay(350);
  setChipString();
  DEBUG.printf("\n[Setup] %s %s\n",PRODUCT, VERSION);

#ifdef DEBUG_MODE
  ticker.attach(0.2, tick);
  digitalWrite(STATUS_LED, HIGH);
#endif

  SPIFFS.begin() ? DEBUG.println("[Setup] FS - Started") : DEBUG.println("[Setup] FS - ERROR");

#ifdef TOTAL_RESET
  {
    SPIFFS.format();
    WiFiManager wifiManager;
    wifiManager.resetSettings();
    // Wifi.disconnect();
  }
#endif

  SPIFFS.exists(CONFIG_FILENAME) ? loadConfig() : onboard();

  if (device_id == nullptr || device_id == "null" || device_id.length() == 0)
  {
    DEBUG.println("[SETUP] FATAL: No device_id");
    ESP.deepSleep(0);
  }

  if (user_id == nullptr || user_id == "null")
    provision();

  DEBUG.print("[Setup] Connecting to Wifi.");
  WiFiManager wifiManager;

  int i = 0;
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1000);
    DEBUG.print(".");
    if (i>20) {
      provision();
    }
    i++;
  };
  DEBUG.println(" Connected.");

  dallas_sensors.begin();

#ifdef DEBUG_MODE
  ota_update();
  ticker.detach();
  digitalWrite(STATUS_LED, LOW);
#endif
}

void onboard()
{
  DEBUG.print("*** *** \n[Onboarding] Formatting SPIFFS... ");
  SPIFFS.format() ? DEBUG.println("Success") : DEBUG.println("Failed");

  connectToWifi(ONBOARD_SSID, ONBOARD_PASSWORD);

  DEBUG.print("[Onboaring] Sensor test: ");
  dallas_sensors.begin();
  (dallas_sensors.getDeviceCount() == 1) ? debug("Passed") : debug("FAILED");
  char temp_buf[64];
  sprintf(temp_buf, "%2.1f", dallas_sensors.getTempCByIndex(0));
  int vcc = analogRead(BATT_ADC_PIN);

  String JSONmessage;
  DynamicJsonDocument doc(400);
  doc["product"] = PRODUCT;
  doc["chip_id"] = device_chipId;
  doc[WATER_TEMP_SENSOR] = temp_buf;
  doc["batt_vcc"] = vcc;
  doc["firmwareVersion"] = VERSION;
  doc["wifi_strength"] = WiFi.RSSI();
  doc["mac_address"] = WiFi.macAddress();
  serializeJson(doc, JSONmessage);
  APIResponse response = HttpJSONStringToEndPoint(JSONmessage, ONBOARD_API_ENDPOINT);
  if (response.code == -1)
  {
    debug("[Onboarding] FAILED - did not recieve a valid device_id.");
    ESP.deepSleep(0);
  }
  else
  {
    // Create config and Write to SPIFFS
    StaticJsonDocument<100> fsDoc;
    fsDoc["device_id"] = response.payload;
    File configFile = SPIFFS.open(CONFIG_FILENAME, "w");
    serializeJson(fsDoc, configFile);
    configFile.close();
    delay(1000);
  }

  SPIFFS.end();
  debug("[Onboarding] SUCCESS!");
  ESP.deepSleep(0);
}

void loadConfig()
{
  File configFile = SPIFFS.open(CONFIG_FILENAME, "r");
  size_t size = configFile.size();
  std::unique_ptr<char[]> buf(new char[size]);

  StaticJsonDocument<100> doc;
  DeserializationError error = deserializeJson(doc, configFile);
  if (error)
  {
    debug("[Load Config] Error deserializing JSON");
    debug(error.c_str(), true);
  }
  configFile.close();
  device_id = doc["device_id"].as<String>();
  user_id = doc["user_id"].as<String>();
}

void provision()
{
  DEBUG.println("\n[Provision] Starting wifiManager");
  delay(300);
  ticker.attach(0.6, tick);

  WiFiManager wifiManager;

  if (user_id == "null")
  {
    user_id.clear();
    DEBUG.println("[Provision] user_id is 'Null'");
    wifiManager.resetSettings();
    delay(1000);
  }

  wifiManager.setConfigPortalTimeout(10 * 60 * 60);
  wifiManager.setSaveConfigCallback(apConfigSaveCallback);
  wifiManager.autoConnect(AP_NAME);

  debug("[Provision] Timed Out");
  ESP.deepSleep(0);
}

//called when wifimanger config is saved
void apConfigSaveCallback()
{
  // look for user
  String JSONmessage;
  StaticJsonDocument<100> doc;
  doc["device_id"] = device_id;
  doc["product"] = PRODUCT;
  serializeJson(doc, JSONmessage);
  APIResponse response = HttpJSONStringToEndPoint(JSONmessage, USER_FOR_DEVICE_API_ENDPOINT);

  if (response.code != -1)
  {
    // payload will contain userID, store it in FS
    StaticJsonDocument<100> fsDoc;
    fsDoc["device_id"] = device_id;
    fsDoc["user_id"] = response.payload;
    File configFile = SPIFFS.open(CONFIG_FILENAME, "w");
    serializeJson(fsDoc, configFile);
    configFile.close();
    delay(1000);
    user_id = response.payload;
    publishDeviceStatus();
    debug("[PROVISION] Success");

    ticker.detach();
    digitalWrite(STATUS_LED, LOW);
  }
  else
  {
    debug("[PROVISION] Failed to find user", true);
    debug(response.payload, true);
  }
}

//gets called when WiFiManager enters configuration mode
void configModeCallback(WiFiManager *myWiFiManager)
{
  ticker.detach();
  ticker.attach(0.2, tick);
}

void loop()
{
  if (user_id != "null")
  {
    publishTemperature();
    device_Maintance();
  }
  else
  {
    debug("[LOOP] User_id is null");
  }

  //Wifi.end();
  //System.sleep(SLEEP_MODE_DEEP,60*15);  //15 mins

  delay(15000);
}

void publishTemperature()
{
  dallas_sensors.requestTemperatures();

  String JSONmessage;
  StaticJsonDocument<400> doc;
  doc["user_id"] = user_id;
  doc["device_id"] = device_id;
  doc["product"] = PRODUCT;
  JsonObject data = doc.createNestedObject("sensors");
  data[WATER_TEMP_SENSOR] = dallas_sensors.getTempCByIndex(0);
  serializeJson(doc, JSONmessage);
  debug(JSONmessage, true);

  HttpJSONStringToEndPoint(JSONmessage, SENSOR_DATA_API_ENDPOINT);
}

void publishDeviceStatus()
{
  StaticJsonDocument<400> doc;
  String JSONmessage;
  doc["user_id"] = user_id;
  doc["device_id"] = device_id;
  doc["product"] = PRODUCT;
  doc["batt_level"] = analogRead(BATT_ADC_PIN);
  doc["wifi_strength"] = WiFi.RSSI();
  doc["firmware_version"] = VERSION;
  serializeJson(doc, JSONmessage);
  debug(JSONmessage, true);

  HttpJSONStringToEndPoint(JSONmessage, DEVICE_STATUS_API_ENDPOINT);
}

APIResponse HttpJSONStringToEndPoint(String JSONString, const char *endPoint)
{
#ifdef DEBUG_MODE
  digitalWrite(STATUS_LED, HIGH);
#endif

  APIResponse response;

  if (WiFi.status() != WL_CONNECTED)
  {
    debug("ERROR: Wifi is not connected");
    response.code = -1;
    return response;
  }

  HTTPClient https;
  BearSSL::WiFiClientSecure secureClient;
  secureClient.setInsecure();
  https.begin(secureClient, API_SERVER_URL, 443, endPoint, true);
  https.addHeader("Content-Type", "application/json");
  response.code = https.POST(JSONString);
  response.payload = https.getString();
  https.end();
  secureClient.stop();

#ifdef DEBUG_MODE
  digitalWrite(STATUS_LED, LOW);
#endif

  if (response.code >= 200 && response.code < 205)
  {
    return response;
  }
  else
  {
    debug("[POST] Invalid response from API", true);
    debug(String(response.code), true);
    debug(response.payload, true);
    response.code = -1;
    return response;
  }
}

void connectToWifi(String ssid, String password)
{
  debug("Connecting to Wifi.");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(300);
  };
  debug("Connected.");
}

void device_Maintance()
{
  if (rebootsSinceLastDevicePost == PUBLISH_DEVICE_CYCLES)
  {
    rebootsSinceLastDevicePost = 0;
    publishDeviceStatus();
    ota_update();
  }

  rebootsSinceLastDevicePost++;
}

void ota_update()
{
  String downloadUrl = getDownloadUrl();
  if (downloadUrl.length() > 0)
    downloadUpdate(downloadUrl);
}

String getDownloadUrl()
{
  HTTPClient https;
  BearSSL::WiFiClientSecure secureClient;
  secureClient.setInsecure();
  String url = FIRMWARE_URL;
  url += String("?version=") + VERSION;
  url += String("&release=") + RELEASE;
  url += String("&product=") + PRODUCT;

  https.begin(secureClient, API_SERVER_URL, 443, url, true);
  int httpCode = https.GET();
  String returnString;

  switch (httpCode)
  {
  case 200:
    returnString = https.getString();
    debug("[OTA] Update available!", true);
    debug(returnString);
    break;

  case 204:
    debug("[OTA] Up to date.", true);
    returnString = "";
    break;

  default:
    debug("[OTA] Failed.", true);
    // debug(httpCode);
    debug(https.getString());
    returnString = "";
  }

  https.end();
  return returnString;
}

bool downloadUpdate(String url)
{
  StaticJsonDocument<200> doc;
  deserializeJson(doc, url);

  HTTPClient https;
  BearSSL::WiFiClientSecure secureClient;
  secureClient.setInsecure();
  https.begin(secureClient, doc["baseURL"], 443, doc["path"], true);

  // start connection and send HTTP header
  int httpCode = https.GET();
  if (httpCode == 0 || httpCode != HTTP_CODE_OK)
  {
    debug("[OTA] Failed - bad HTTP code", true);
    return false;
  }

  size_t contentLength = https.getSize();

  if (contentLength == 0)
  {
    debug("There was no content in the response", true);
    return false;
  }

  if (!Update.begin(contentLength, U_FLASH))
  {
    debug("Not enough space to begin OTA", true);
    return false;
  }

  size_t written = Update.writeStream(https.getStream());
  (written == contentLength) ? DEBUG.println("Written : " + String(written) + " successfully") : DEBUG.println("Written only : " + String(written) + "/" + String(contentLength));

  Update.setMD5(doc["md5Hash"]);

  if (!Update.end())
  {
    debug(String(Update.getError()), true);
    return false;
  }

  ticker.detach();

  if (Update.isFinished())
  {
    debug("Update successfully completed. Rebooting.", true);
    delay(2000);
    WiFi.forceSleepBegin();
    wdt_reset();
    ESP.restart();
    while (1)
      wdt_reset();
    //ESP.reset();
    //delay(2000);
    return true;
  }

  debug("Update not finished. Something went wrong!", true);
  debug(String(Update.getError()), true);
  return false;
}

void tick()
{
  //toggle state
  int state = digitalRead(STATUS_LED); // get the current state of GPIO1 pin
  digitalWrite(STATUS_LED, !state);    // set pin to the opposite state
}

void debug(String string, bool publish)
{
  //TODO: if no Wifi, store in array in FS.
  DEBUG.println(string);
  return;

  if (publish)
  {
    StaticJsonDocument<300> doc;
    String JSONmessage;
    doc["device_id"] = device_id;
    doc["product"] = PRODUCT;
    doc["debugString"] = string;
    serializeJson(doc, JSONmessage);
    HttpJSONStringToEndPoint(JSONmessage, DEBUG_API_ENDPOINT);
    delay(1000);
  }
}

/*   STATES
Not_onboarded: config.json does not exist and doesn't contains serial_id
onboard: config.json exists but does not contain user_id && wifi credentials are not setup
provisioned: config.json excists, contains serial_id and user_id and wifi creds are setup
has_errors: normally wifi connection issues.

enum states
{
  not_onboarded,
  onboarded,
  provisioned,
  has_errors
}
*/

/*
void FSDirectory()
{
  String str = "Directory: ";
  Dir dir = SPIFFS.openDir("/");
  while (dir.next())
  {
    str += dir.fileName();
    str += " / ";
    str += dir.fileSize();
    str += "\r\n";
  }
  DEBUG.print(str);
}

void FSInfoPeek()
{
  FSInfo fs_info;
  SPIFFS.info(fs_info);
  DEBUG.print("SPIFSS totalBytes :");
  DEBUG.print(fs_info.totalBytes);
}
*/