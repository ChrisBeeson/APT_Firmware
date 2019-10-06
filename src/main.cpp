#include "Arduino.h"
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


//
// This is the ANDRIS Pool Themometer (APT2019) wifi battery powered floating water temperature sensor.
//

//TODO:
// OTA updates - Testing
// batt_level
// device posts X
// wifi_level X
// provisioning
// eeprom storage
// deep sleep

// Versioning

//#define DEBUG_ESP_HTTP_CLIENT 1

#define PRODUCT "APT2019"
#define CURRENT_VERSION VERSION
#define VARIANT "d1"

#define FIRMWARE_URL "/getFirmwareDownloadUrl"
#define API_SERVER_URL "us-central1-chas-c2689.cloudfunctions.net"
#define SENSOR_DATA_API_ENDPOINT "/sensorData"
#define DEVICE_STATUS_API_ENDPOINT "/deviceStatus"

#define STORAGE_BASE_URL "storage.googleapis.com"

#define USE_SERIAL Serial

// Hardware
#define TEMP_SENSOR_BUS D3
#define ANALOG_BUS A0

OneWire oneWire(TEMP_SENSOR_BUS);
DallasTemperature sensors(&oneWire);

double currentTemp = 0.0;

// System

const char *sensorType = "water_temp"; // only 1 sensor
char device_chipId[13];
double batt_level;
int A0sensorValue = 0;
int errorsSinceLastDevicePost = 0;

// User
const char *userId = "testUserA";
char ssid[] = "Galactica";
char password[] = "archiefifi";
//TODO: Wifi needs to be encryped and loaded from EEPROM.

void setChipString();
int wifiStrengthInBars();
void connectToWifi();
bool HttpJSONStringToEndPoint(String JSONString, const char *endPoint);
void publishTemp();
void device_Maintance();
void publishDeviceStatus();
String getDownloadUrl();
bool downloadUpdate(String url);

void setup()
{
  pinMode(LED_BUILTIN, OUTPUT);

  Serial.begin(115200);
  delay(10);
  Serial.println('\n');
  setChipString();

  // Are we freshly flashed? And need to send chipID to DB? - This will be done over usb. Test sensor & battery.
  // Are we provisioned?  Ie. Have a username, ssid and password?

  sensors.begin(); // Start sampling Temperature

  connectToWifi();
}

void loop()
{
  digitalWrite(LED_BUILTIN, HIGH);

  sensors.requestTemperatures(); // sample temp
  double tempCheck = sensors.getTempCByIndex(0);

  currentTemp = tempCheck;
  publishTemp();

  if (tempCheck > -50.0 && tempCheck < 60.0)
  {
    currentTemp = tempCheck;
    // publishTemp();
  }
  else
  {
    Serial.println("Error: Temp out of bounds");
    Serial.println(tempCheck);

    // locate devices on the bus
    Serial.print("Found ");
    Serial.print(sensors.getDeviceCount(), DEC);
    Serial.println(" devices.");
  }

  // every X amount of data samples run device_maintance to check batt / wifi / updates
  device_Maintance();

  // Close wifi

  //Wifi.end();

  // Sleep

  //  System.sleep(SLEEP_MODE_DEEP,60*15);  //15 mins
  digitalWrite(LED_BUILTIN, LOW);

  delay(15000);
}

void setChipString()
{
  uint64_t chipid;
  chipid = ESP.getChipId();
  uint16_t chip = (uint16_t)(chipid >> 32);
  snprintf(device_chipId, 13, "%04X%08X", chip, (uint32_t)chipid);
}

void publishTemp()
{
  char temp_buf[64];
  sprintf(temp_buf, "%2.1f", currentTemp);
  Serial.println(currentTemp);

  // Create JSON String
  DynamicJsonDocument doc(400);

  doc["user_id"] = userId;
  doc["device_id"] = device_chipId;
  doc["sensor"] = sensorType;
  doc["data"] = temp_buf;

  // int lengthSimple = doc.measureLength();
  String JSONmessage;
  serializeJson(doc, JSONmessage);
  Serial.println(JSONmessage);

  if (!HttpJSONStringToEndPoint(JSONmessage, SENSOR_DATA_API_ENDPOINT))
  {
    Serial.println("JSON Post failed");
    errorsSinceLastDevicePost++;
  }
}

void publishDeviceStatus()
{
  A0sensorValue = analogRead(ANALOG_BUS); // batt_ level from A0

  DynamicJsonDocument doc(400);
  doc["user_id"] = userId;
  doc["device_id"] = device_chipId;
  doc["product"] = PRODUCT;
  doc["batt_level"] = A0sensorValue;
  doc["wifi_strength"] = wifiStrengthInBars();
  doc["errorsSinceLastDevicePost"] = errorsSinceLastDevicePost;
  doc["firmwareVersion"] = CURRENT_VERSION;

  String JSONmessage;
  serializeJson(doc, JSONmessage);
  Serial.println(JSONmessage);

  if (!HttpJSONStringToEndPoint(JSONmessage, DEVICE_STATUS_API_ENDPOINT))
  {
    Serial.println("JSON Post failed");
    errorsSinceLastDevicePost++;
  }
  else
  {
    errorsSinceLastDevicePost = 0;
  }
}

bool HttpJSONStringToEndPoint(String JSONString, const char *endPoint)
{
  if (WiFi.status() == WL_CONNECTED)
  {
    HTTPClient https;
    BearSSL::WiFiClientSecure secureClient;
    secureClient.setInsecure();
    https.begin(secureClient, API_SERVER_URL, 443, endPoint, true);
    https.addHeader("Content-Type", "application/json");

    int httpResponseCode = https.POST(JSONString);

    if (httpResponseCode > 0)
    {
      String response = https.getString();
      Serial.println(httpResponseCode);
      Serial.println(response);
    }
    else
    {
      Serial.print("Error on sending PUT Request: ");
      Serial.println(httpResponseCode);
      String response = https.getString();
      Serial.println(response);
      errorsSinceLastDevicePost++;
      return false;
    }
    https.end();
    secureClient.stop();
    return true;
  }
  else
  {
    Serial.println("Trying to send JSON, but Wifi is not connected");
    errorsSinceLastDevicePost++;
    return false;
  }
}

int wifiStrengthInBars()
{
  long RSSI = WiFi.RSSI();
  int bars;

  if ((RSSI > -55))
  {
    bars = 5;
  }
  else if ((RSSI < -55) & (RSSI > -65))
  {
    bars = 4;
  }
  else if ((RSSI < -65) & (RSSI > -70))
  {
    bars = 3;
  }
  else if ((RSSI < -70) & (RSSI > -78))
  {
    bars = 2;
  }
  else if ((RSSI < -78) & (RSSI > -82))
  {
    bars = 1;
  }
  else
  {
    bars = 0;
  }
  return bars;
}

void connectToWifi()
{
  WiFi.begin(ssid, password);
  Serial.print("Connecting to ");
  Serial.print(ssid);
  Serial.println(" ...");

  int i = 0;
  while (WiFi.status() != WL_CONNECTED)
  { // Wait for the Wi-Fi to connect
    delay(1000);
    Serial.print(++i);
    Serial.print(' ');
  }

  Serial.println('\n');
  Serial.println("Connection established!");
  Serial.print("IP address:\t");
  Serial.println(WiFi.localIP()); // Send the IP address of the ESP8266 to the computer
}

void device_Maintance()
{
  publishDeviceStatus();

  // Check if we need to download a new version
  String downloadUrl = getDownloadUrl();
  if (downloadUrl.length() > 0)
  {
    bool success = downloadUpdate(downloadUrl);
    if (!success)
    {
      USE_SERIAL.println("Error updating device");
    }
  }
}

/* 
 * Check if needs to update the device and returns the download url.
 */
String getDownloadUrl()
{
  USE_SERIAL.print("Checking for new firmware version. ");

  HTTPClient https;
  BearSSL::WiFiClientSecure secureClient;
  secureClient.setInsecure();
  String downloadUrl;

  USE_SERIAL.print(". ");
  String url = FIRMWARE_URL;
  url += String("?version=") + CURRENT_VERSION;
  url += String("&variant=") + VARIANT;
  url += String("&product=") + PRODUCT;
  https.begin(secureClient, API_SERVER_URL, 443, url, true);

  USE_SERIAL.print(". ");
  int httpCode = https.GET();
  USE_SERIAL.print(". ");

  if (httpCode > 0)
  {
    String response = https.getString();
    USE_SERIAL.printf(". HTTP Response:");
    USE_SERIAL.println(response);

    if (httpCode == HTTP_CODE_OK)
    {
      USE_SERIAL.println("Theres a new version to download.");
      String payload = https.getString();
      USE_SERIAL.println(payload);
      downloadUrl = payload;
      return downloadUrl;
    }
    else
    {
      USE_SERIAL.println("Device is up to date.");
    }
  }
  else
  {
    USE_SERIAL.printf(" ! FAILED, error: %s\n", https.errorToString(httpCode).c_str());
    return "";
  }
  https.end();
  return downloadUrl;
}

bool downloadUpdate(String url)
{
        USE_SERIAL.println("Beginning download");
                USE_SERIAL.println(url);
  HTTPClient https;
  BearSSL::WiFiClientSecure secureClient;
  secureClient.setInsecure();

  https.begin(secureClient, STORAGE_BASE_URL, 443, url, true);

    USE_SERIAL.printf(". ");
  // start connection and send HTTP header
  int httpCode = https.GET();

    USE_SERIAL.print("HTTP Code:");
  USE_SERIAL.println(httpCode);
      String response = https.getString();
    USE_SERIAL.println(response);

  if (httpCode > 0)
  {
    // HTTP header has been send and Server response header has been handled
    USE_SERIAL.printf("[HTTP] GET... code: %d\n", httpCode);

    // file found at server
    if (httpCode == HTTP_CODE_OK)
    {

      size_t contentLength = https.getSize();
      USE_SERIAL.println("contentLength : " + String(contentLength));

      if (contentLength > 0)
      {
        bool canBegin = Update.begin(contentLength, U_FLASH);
        if (canBegin)
        {
          WiFiClient stream = https.getStream();
                    USE_SERIAL.print("stream: ");
          USE_SERIAL.println(stream);
          USE_SERIAL.println("Begin OTA. This may take 2 - 5 mins to complete. Things might be quiet for a while.. Patience!");
          size_t written = Update.writeStream(stream);

          if (written == contentLength)
          {
            USE_SERIAL.println("Written : " + String(written) + " successfully");
          }
          else
          {
            USE_SERIAL.println("Written only : " + String(written) + "/" + String(contentLength) + ". Retry?");
          }

          if (Update.end())
          {
            USE_SERIAL.println("OTA done!");
            if (Update.isFinished())
            {
              USE_SERIAL.println("Update successfully completed. Rebooting.");
              ESP.restart();
              return true;
            }
            else
            {
              USE_SERIAL.println("Update not finished? Something went wrong!");
              return false;
            }
          }
          else
          {
            USE_SERIAL.println("Error Occurred. Error #: " + String(Update.getError()));
            return false;
          }
        }
        else
        {
          USE_SERIAL.println("Not enough space to begin OTA");
          //   client.flush();    <- not sure what flush is
          return false;
        }
      }
      else
      {
        USE_SERIAL.println("There was no content in the response");
        //   client.flush();
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
