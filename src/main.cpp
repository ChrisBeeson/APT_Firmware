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
#define PUBLISH_DEVICE_CYCLES 0 // number of restarts before publishing device status

#define DEBUG Serial
//#define DEBUG_UPDATER Serial
//#define TOTAL_RESET

OneWire oneWire(TEMP_SENSOR_PIN);
DallasTemperature dallas_sensors(&oneWire);
Ticker ticker;

// System
char device_chipId[13];
int rebootsSinceLastDevicePost = 0;
String device_id;
String user_id;

struct httpResponse {
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
String HttpJSONStringToEndPoint(String JSONString, const char *endPoint);
void publishTemperature();
void device_Maintance();
void publishDeviceStatus();
String getDownloadUrl();
bool downloadUpdate(String url);
void debug(String string, bool publish = false);
void apConfigSaveCallback();



void setup()
{
  pinMode(STATUS_LED, OUTPUT);
  Serial.begin(115200);
  delay(350);
  setChipString();
  //digitalWrite(STATUS_LED, HIGH);

  SPIFFS.begin() ? DEBUG.println("File System: Started") : DEBUG.println("File System: ERROR");

#ifdef TOTAL_RESET
{
  SPIFFS.format();
  WiFiManager wifiManager;
  wifiManager.resetSettings();
 // Wifi.disconnect();
}
#endif

  SPIFFS.exists(CONFIG_FILENAME) ? loadConfig() : onboard();

  if (device_id == nullptr || device_id == "null")
  {
    DEBUG.println("FATAL ERROR: Device does not have a device_id");
    ESP.deepSleep(0);
  }

  if (user_id == nullptr || user_id == "null")
    provision();

  DEBUG.print("Connecting to Wifi.");

  WiFiManager wifiManager;
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(300);
    DEBUG.print(".");
  };
  DEBUG.println(" Connected.");

  debug(VERSION, true);
  dallas_sensors.begin(); // Start sampling Temperature
  ticker.attach(0.6, tick);
}

void onboard()
{
  DEBUG.print("*** Onboarding *** \nFormatting SPIFFS...");
  SPIFFS.format() ? DEBUG.println("Success") : DEBUG.println("Failed");

  connectToWifi(ONBOARD_SSID, ONBOARD_PASSWORD);

  DEBUG.print("Confirming Sensor count...");
  dallas_sensors.begin();
  (dallas_sensors.getDeviceCount() == 1) ? debug("Success!") : debug("FAILED");
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
  String response = HttpJSONStringToEndPoint(JSONmessage, ONBOARD_API_ENDPOINT);
  if (response == "")
  {
    DEBUG.println("FATAL ERROR: Onboarding did not recieve a valid device_id");
    ESP.deepSleep(0);
  }
  else
  {
    // Create config and Write to SPIFFS
    StaticJsonDocument<100> fsDoc;
    fsDoc["device_id"] = response;
    File configFile = SPIFFS.open(CONFIG_FILENAME, "w");
    serializeJson(fsDoc, configFile);
    configFile.close();
    delay(1000);
  }

  SPIFFS.end();
  debug("Onboarding SUCCESS");
  ESP.deepSleep(0);
}

void loadConfig()
{
  File configFile = SPIFFS.open(CONFIG_FILENAME, "r");
  DEBUG.println(configFile);
  size_t size = configFile.size();
  std::unique_ptr<char[]> buf(new char[size]);

  StaticJsonDocument<100> doc;
  DeserializationError error = deserializeJson(doc, configFile);
  if (error) debug(error.c_str(), true);
  
  configFile.close();
  device_id = doc["device_id"].as<String>();
  user_id = doc["user_id"].as<String>();
}

void provision()
{
  DEBUG.println("\nProvision - starting wifiManager");
  delay(300);
  ticker.attach(0.6, tick);

  WiFiManager wifiManager;

  if (user_id == "null") {
    user_id.clear();
    DEBUG.println("user_id is Null, Cleared user_id and resetting AP");
    wifiManager.resetSettings();
    delay(1000);
  }

  wifiManager.setConfigPortalTimeout(10*60*60);
  wifiManager.setSaveConfigCallback(apConfigSaveCallback);
  wifiManager.autoConnect(AP_NAME);
  
  debug("Provision Timed Out");
  ESP.deepSleep(0);
}

//called when wifimanger config is saved 
void apConfigSaveCallback() {

  DEBUG.println("AP config recieved.  Searching for User looking for device on same Public IP");

  String JSONmessage;
  StaticJsonDocument<100> doc;
  doc["device_id"] = device_id;
  doc["product"] = PRODUCT;
  serializeJson(doc, JSONmessage);
  DEBUG.println(JSONmessage);

  String response = HttpJSONStringToEndPoint(JSONmessage, USER_FOR_DEVICE_API_ENDPOINT);
  DEBUG.println(response);

  if (response != "" )
  {
    StaticJsonDocument<100> fsDoc;
    fsDoc["device_id"] = device_id;
    fsDoc["user_id"] = response;
    File configFile = SPIFFS.open(CONFIG_FILENAME, "w");
    serializeJson(fsDoc, configFile);
    configFile.close();
    delay(1000);
    user_id = response;
    publishDeviceStatus();

    ticker.detach();
    digitalWrite(STATUS_LED, LOW);
  } else {
    debug("[PROVISION] Could Not Find User", true);
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
    debug("[LOOP] User_id is null - not publishing anything !", true);
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
  DEBUG.println(JSONmessage);

  if (HttpJSONStringToEndPoint(JSONmessage, SENSOR_DATA_API_ENDPOINT) != "")
  {
    DEBUG.println(" Success");
  }
}

void publishDeviceStatus()
{
  DEBUG.println("Publish Device Status: ");
  StaticJsonDocument<400> doc;
  String JSONmessage;
  doc["user_id"] = user_id;
  doc["device_id"] = device_id;
  doc["product"] = PRODUCT;
  doc["batt_level"] = analogRead(BATT_ADC_PIN);
  doc["wifi_strength"] = WiFi.RSSI();
  doc["firmware_version"] = VERSION;
  serializeJson(doc, JSONmessage);
  DEBUG.println(JSONmessage);
  if (HttpJSONStringToEndPoint(JSONmessage, DEVICE_STATUS_API_ENDPOINT) != "")
    debug("Success\n");
}

void debug(String string, bool publish)
{

  //TODO: if no Wifi, store in array in FS.
  DEBUG.println(string);

  if (publish)
  {
    StaticJsonDocument<300> doc;
    String JSONmessage;
    doc["device_id"] = device_id;
    doc["product"] = PRODUCT;
    doc["debugString"] = string;
    serializeJson(doc, JSONmessage);
    HttpJSONStringToEndPoint(JSONmessage, DEBUG_API_ENDPOINT);
  }
}

String HttpJSONStringToEndPoint(String JSONString, const char *endPoint)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    DEBUG.println("ERROR: Wifi is not connected");
    return "";
  }

  HTTPClient https;
  BearSSL::WiFiClientSecure secureClient;
  secureClient.setInsecure();
  https.begin(secureClient, API_SERVER_URL, 443, endPoint, true);
  https.addHeader("Content-Type", "application/json");

  int httpResponseCode = https.POST(JSONString);
  String response = https.getString();

  https.end();
  secureClient.stop();

  if (httpResponseCode >= 200 && httpResponseCode < 205)
  {
    return response;
  }
  else
  {
    DEBUG.print("Error! ");
    DEBUG.println(httpResponseCode);
    DEBUG.println(response);
    return "";
  }
}

void connectToWifi(String ssid, String password)
{
  DEBUG.print("Connecting to Wifi.");
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(300);
    DEBUG.print(".");
  };

  DEBUG.println(" Connected.");
}

void device_Maintance()
{
  if (rebootsSinceLastDevicePost == PUBLISH_DEVICE_CYCLES)
  {
    rebootsSinceLastDevicePost = 0;
    publishDeviceStatus();

    String downloadUrl = getDownloadUrl();
    if (downloadUrl.length() > 0)
      downloadUpdate(downloadUrl);
  }

  rebootsSinceLastDevicePost++;
}

/* 
 * Check if needs to update the device and returns the download url.
 */
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
    debug("New firmware available!", true);
    break;

  case 204:
    debug("Device is up to date.", true);
    returnString = "";
    break;

  default:

    debug("Unable to download firmware URL, error: %s\n", https.errorToString(httpCode).c_str());
    returnString = "";
  }
  https.end();
  return returnString;
}

bool downloadUpdate(String url)
{
  DEBUG.println(url);
  DEBUG.print("Downloading Update... ");
  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, url);

  if (error)
  {
    DEBUG.print(error.c_str());
    return false;
  }

  HTTPClient https;
  BearSSL::WiFiClientSecure secureClient;
  secureClient.setInsecure();
  https.begin(secureClient, doc["baseURL"], 443, doc["path"], true);

  // start connection and send HTTP header
  int httpCode = https.GET();

  if (httpCode == 0 || httpCode != HTTP_CODE_OK)
  {
    DEBUG.printf("Failed HTTPCode: %i", httpCode);
    return false;
  }

  size_t contentLength = https.getSize();

  if (contentLength == 0)
  {
    DEBUG.println("There was no content in the response");
    return false;
  }

  //Update.setMD5(doc["md5Hash"]);

  if (!Update.begin(contentLength, U_FLASH))
  {
    DEBUG.println("Not enough space to begin OTA");
    return false;
  }

  size_t written = Update.writeStream(https.getStream());
  (written == contentLength) ? DEBUG.println("Written : " + String(written) + " successfully") : DEBUG.println("Written only : " + String(written) + "/" + String(contentLength));

  if (!Update.end())
  {
    DEBUG.println("Error Occurred. Error #: " + String(Update.getError()));
    return false;
  }
  /*
  if (!Update.setMD5(doc["md5Hash"])) {
      DEBUG.println("Failed MD5 checksum");
      DEBUG.println(Update.)
  }
*/
  if (Update.isFinished())
  {
    DEBUG.println("Update successfully completed. Rebooting.");
    delay(350);
    ESP.restart();
    return true;
  }

  DEBUG.println("Update not finished. Something went wrong!");
  DEBUG.println("Error Occurred. Error #: " + String(Update.getError()));
  return false;
}

void tick()
{
  //toggle state
  int state = digitalRead(STATUS_LED); // get the current state of GPIO1 pin
  digitalWrite(STATUS_LED, !state);    // set pin to the opposite state
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