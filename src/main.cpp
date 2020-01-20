
#include "Esp.h"
#include <WiFiClientSecure.h>
#include <WiFiClientSecureBearSSL.h>
#include <ESP8266WiFiMulti.h>
#include <WiFiUdp.h>
#include <ESP8266WiFiType.h>
#include <CertStoreBearSSL.h>
#include <ESP8266WiFiAP.h>
#include <BearSSLHelpers.h>
#include <ESP8266WiFiScan.h>
#include <ESP8266WiFiGeneric.h>
#include <ESP8266WiFiSTA.h>
#include <WiFiClientSecureAxTLS.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <string.h>
#include <FS.h>
#include <WiFiManager.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>

//
// ANDRIS Pool Thermometer (APT01) wifi battery powered floating water temperature sensor.
//

//TODO:
// [X] OTA updates Xf
// [ ] OTA console monitoring
// [ ] batt_level
// [X] device posts
// [X] wifi_level
// [ ] provisioning
// [x] Manufacturing onboarding
// [x] file storage
// [x] deep sleep
// [ ] optimise battery - Compress and limit wifi broadcast, limited leds, sensors off after initial reading etc.
//

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

#define PUBLISH_DEVICE_CYCLES 2 // number of restarts before publishing device status

#define DEBUG Serial

OneWire oneWire(TEMP_SENSOR_PIN);
DallasTemperature dallas_sensors(&oneWire);

// System
char device_chipId[13];

int errorsSinceLastDevicePost = 0;
int rebootsSinceLastDevicePost = 0;

// User
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
  delay(50);
  setChipString();

  SPIFFS.begin() ? DEBUG.println("File System Started") : DEBUG.println("File System Begin Error");
  SPIFFS.exists(CONFIG_FILENAME) ? loadConfig() : onboard();

  if (device_id == nullptr || device_id == "null")
  {
    DEBUG.println("FATAL ERROR: Device does not have a device_id");
    ESP.deepSleep(0);
  }

  if (user_id == "null")
    provision();

  if (WiFi.status() != WL_CONNECTED)
  {
    WiFiManager wifiManager;
    while (WiFi.status() != WL_CONNECTED)
    {
      delay(100);
    };
  }

  //connectToWifi();
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
    DEBUG.println(F("Error: deserializeJson"));
    DEBUG.println(error.c_str());
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
  DEBUG.print("Publish Device Status...");
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
  DEBUG.print("JSON POST... ");
  if (WiFi.status() == WL_CONNECTED)
  {
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

    return "";
  }
  else
  {
    DEBUG.println("ERROR: Wifi is not connected");
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

  DEBUG.print("Checking for new firmware version...");
  https.begin(secureClient, API_SERVER_URL, 443, url, true);
  int httpCode = https.GET();
  https.end();

  if (httpCode > 0)
  {
    if (httpCode == HTTP_CODE_OK)
    {
      DEBUG.print("New firmware available: ");
      String payload = https.getString();
      DEBUG.println(payload);
      return payload;
    }
    else
    {
      DEBUG.println("Device is up to date.");
      return "";
    }
  }
  else
  {
    DEBUG.printf("Unable to download firmware URL, error: %s\n", https.errorToString(httpCode).c_str());
    return "";
  }
}

bool downloadUpdate(String url)
{
  DEBUG.print("Downloading Update...");
  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, url);

  if (error)
  {
    DEBUG.print(F("deserializeJson() failed: "));
    DEBUG.println(error.c_str());
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
  if (httpCode > 0)
  {
    // file found at server
    if (httpCode == HTTP_CODE_OK)
    {
      size_t contentLength = https.getSize();

      if (contentLength > 0)
      {
        bool canBegin = Update.begin(contentLength, U_FLASH);
        if (canBegin)
        {
          DEBUG.print("Beginning OTA update...");
          size_t written = Update.writeStream(https.getStream());
          (written == contentLength) ? DEBUG.println("Written : " + String(written) + " successfully") : DEBUG.println("Written only : " + String(written) + "/" + String(contentLength));

          if (Update.end())
          {
            DEBUG.println("Download complete!");
            if (Update.isFinished())
            {
              DEBUG.println("Update successfully completed. Rebooting.");
              ESP.restart();
              return true;
            }
            else
            {
              DEBUG.println("Update not finished. Something went wrong!");
              return false;
            }
          }
          else
          {
            DEBUG.println("Error Occurred. Error #: " + String(Update.getError()));
            return false;
          }
        }
        else
        {
          DEBUG.println("Not enough space to begin OTA");
          return false;
        }
      }
      else
      {
        DEBUG.println("There was no content in the response");
        return false;
      }
    }
    else
    {
      return false;
    }
  }
  else
  {
    return false;
  }
}
