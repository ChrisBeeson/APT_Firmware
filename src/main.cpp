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

#define FIRMWARE_URL "/getFirmwareDownloadUrl"
#define API_SERVER_URL "us-central1-chas-c2689.cloudfunctions.net" //TODO: move to config file
#define ONBOARD_API_ENDPOINT "/onboardDevice"
#define USER_FOR_DEVICE_API_ENDPOINT "/userForDevice"
#define SENSOR_DATA_API_ENDPOINT "/sensorData"
#define DEVICE_STATUS_API_ENDPOINT "/deviceStatus"
#define DEBUG_API_ENDPOINT "/debug"
#define CONFIG_FILENAME "/config.json"
#define ONBOARD_SSID "Galactica"
#define ONBOARD_PASSWORD "archiefifi"
#define AP_NAME "Pool Thermometer"
#define WATER_TEMP_SENSOR "water_temp"
#define PUBLISH_DEVICE_CYCLES 0    // number of restarts before publishing device status
#define DEBUG Serial
#define DEBUG_UPDATER Serial

OneWire oneWire(TEMP_SENSOR_PIN);
DallasTemperature dallas_sensors(&oneWire);

// System
char device_chipId[13];
int errorsSinceLastDevicePost = 0;
int rebootsSinceLastDevicePost = 0;
String device_id;
String user_id;

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
bool connectToWifi(String ssid, String password);
String HttpJSONStringToEndPoint(String JSONString, const char *endPoint);
void publishTemperature();
void device_Maintance();
void publishDeviceStatus();
String getDownloadUrl();
bool downloadUpdate(String url);

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

void setup()
{
  pinMode(STATUS_LED, OUTPUT);
  Serial.begin(115200);
  delay(350);
  setChipString();
  DEBUG.printf("\nAPT Firmware Version: %s\n", VERSION);

   DEBUG.print("NOT OTA");

  SPIFFS.begin() ? DEBUG.println("File System: Started") : DEBUG.println("File System: ERROR");
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
  dallas_sensors.begin(); // Start sampling Temperature
}

void onboard()
{
  DEBUG.print("*** Onboarding *** \nFormatting SPIFFS...");
  SPIFFS.format() ? DEBUG.println("Success") : DEBUG.println("Failed");

  DEBUG.print("Connecting to WIFI.");
  WiFi.begin(ONBOARD_SSID, ONBOARD_PASSWORD);
  while (WiFi.status() != WL_CONNECTED)
  {
    DEBUG.print(".");
    delay(300);
  }
  DEBUG.println("Success");

  DEBUG.print("Confirming Sensor count...");
  dallas_sensors.begin();
  (dallas_sensors.getDeviceCount() == 1) ? DEBUG.println("Success") : DEBUG.println("FAILED");
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
    DEBUG.print(F("Serializing to file. Size = "));
    uint16 size = serializeJson(fsDoc, configFile);
    DEBUG.println(size);
    configFile.close();
    delay(1000);
  }

  SPIFFS.end();
  DEBUG.println("Entering Deep Sleep");
  ESP.deepSleep(0);
}


void loadConfig()
{
  DEBUG.print("Load Config.json....");
  File configFile = SPIFFS.open(CONFIG_FILENAME, "r");

  size_t size = configFile.size();
  std::unique_ptr<char[]> buf(new char[size]);

  StaticJsonDocument<100> doc;
  DeserializationError error = deserializeJson(doc, configFile);
  if (error)
  {
    DEBUG.println(F("Error: deserializeJson: "));
    DEBUG.print(error.c_str());
  }
  DEBUG.print(F("serializeJson = "));
  serializeJson(doc, Serial);
  DEBUG.println("\n");
  configFile.close();

  device_id = doc["device_id"].as<String>();
  user_id = doc["user_id"].as<String>();
}

void provision()
{
  DEBUG.println("\nNo USER_ID, We need to provision - starting wifiManager");
  delay(300);
  WiFiManager wifiManager;
  //wifiManager.resetSettings();
  wifiManager.autoConnect(AP_NAME);
  DEBUG.println("Connected.... Trying to retreive User for this device");
  String JSONmessage;
  StaticJsonDocument<100> doc;
  doc["device_id"] = device_id;
  serializeJson(doc, JSONmessage);
  DEBUG.println(JSONmessage);

  String response = HttpJSONStringToEndPoint(JSONmessage, USER_FOR_DEVICE_API_ENDPOINT);
  if (response != "")
  {
    StaticJsonDocument<100> fsDoc;
    fsDoc["device_id"] = device_id;
    fsDoc["user_id"] = response;
    File configFile = SPIFFS.open(CONFIG_FILENAME, "w");
    DEBUG.print(F("Serializing to file. Size = "));
    uint16 size = serializeJson(fsDoc, configFile);
    DEBUG.println(size);
    configFile.close();
    delay(1000);
    user_id = response;
    publishDeviceStatus();
  }
  DEBUG.println(response);
}

void loop()
{
  //digitalWrite(LED_BUILTIN, HIGH);

  if (user_id != "null")
  {
    publishTemperature();
    device_Maintance();
  }
  else
  {
    DEBUG.println("! User_id is null - not publishing anything !");
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
    DEBUG.println("Success");
}

String HttpJSONStringToEndPoint(String JSONString, const char *endPoint)
{
  DEBUG.print("Posting... ");

  if (WiFi.status() != WL_CONNECTED) {
    DEBUG.println("ERROR: Wifi is not connected");
    errorsSinceLastDevicePost++;
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
      errorsSinceLastDevicePost++;
      return "";
    }

}

bool connectToWifi(String ssid, String password)
{
  WiFi.begin(ssid, password);
  int i = 0;
  while (WiFi.status() != WL_CONNECTED)
  { // Wait for the Wi-Fi to connect
    i++;
    if (i == 1000)
    {
      DEBUG.println("Unable to connect to Wifi after 1000 attempts");
      // TODO: Decide how to handle this
      return false;
    }
    delay(1000);
  }
  DEBUG.println("Wifi Connection Established!");

  return true;
}

void device_Maintance()
{
  if (rebootsSinceLastDevicePost == PUBLISH_DEVICE_CYCLES)
  {
    rebootsSinceLastDevicePost = 0;
    publishDeviceStatus();

    // OTA Update - is there a new version?
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

  DEBUG.print("Checking for new firmware version... ");
  https.begin(secureClient, API_SERVER_URL, 443, url, true);
  int httpCode = https.GET();
  String returnString;

  switch (httpCode)
  {
  case 200:
    returnString = https.getString();
    DEBUG.println("New firmware available! ");
    break;

  case 204:
    DEBUG.println("Device is up to date.");
    returnString = "";
    break;

  default:
    DEBUG.printf("Unable to download firmware URL, error: %s\n", https.errorToString(httpCode).c_str());
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

  const char *baseURL = doc["baseURL"];
  const char *path = doc["path"];

  HTTPClient https;
  BearSSL::WiFiClientSecure secureClient;
  secureClient.setInsecure();
  https.begin(secureClient, baseURL, 443, path, true);

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

  bool canBegin = Update.begin(contentLength, U_FLASH);
  if (!canBegin)
  {
    DEBUG.println("Not enough space to begin OTA");
    return false;
  }

  size_t written = Update.writeStream(https.getStream());
  (written == contentLength) ? DEBUG.println("Written : " + String(written) + " successfully") : DEBUG.println("Written only : " + String(written) + "/" + String(contentLength));

  if (Update.hasError())
  {
    DEBUG.println("Error Occurred. Error #: " + String(Update.getError()));
    return false;
  }

  DEBUG.println("Download complete!");

  if (Update.isFinished())
  {
    DEBUG.println("Update successfully completed. Rebooting.");
    delay(350);
    ESP.restart();
    return true;
  }

  DEBUG.println("Update not finished. Something went wrong!");
  return false;
}
