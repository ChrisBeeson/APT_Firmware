
This firmware powers a WiFi-enabled floating water sensor for pools using an ESP8266 microcontroller. It reads water temperature using a Dallas temperature sensor and sends the data securely to a cloud API endpoint.

---

## 💡 Features

- Connects to WiFi using WiFiManager
- Uses a Dallas DS18B20 temperature sensor via OneWire
- Sends data to Google Cloud Functions endpoints
- Supports OTA firmware updates
- Double reset detection to enter configuration mode
- Persistent config stored in SPIFFS
- Device onboarding and user-device linking

---

## 📦 Project Structure

- `src/main.cpp` - Main firmware logic
- `.vscode/` - VS Code settings
- `platformio.ini` - PlatformIO project config
- `include/` & `lib/` - For headers/libraries (empty with README placeholders)
- `extra_script.py` - PlatformIO build script
- `cloudbuild.yaml` & `.travis.yml` - CI/CD configs

---

## 🔧 Setup Instructions

1. Clone the repository
2. Flash using [PlatformIO](https://platformio.org/)
3. Power on the device – it will:
   - Attempt to connect to WiFi
   - Launch its own Access Point for setup if needed
4. Reads temperature and posts JSON payload to:

5. Supports OTA updates via ArduinoOTA

---

## ⚙️ Configuration

Config is stored in SPIFFS as `/config.json`.

WiFi AP mode is enabled with:
- SSID: `Pool Thermometer`
- Default credentials: `Galactica / archiefifi`

---

## 🔒 Endpoints

Defined endpoints:
- `/sensorData`: Post temperature readings
- `/onboardDevice`: Initial device onboarding
- `/userForDevice`: Link user to device
- `/deviceStatus`: Device status report
- `/consoleLog`: Debug log reporting

---

## 🧪 Testing

To simulate data posting, test endpoint responses with `curl` or Postman. Make sure your cloud functions are properly deployed and HTTPS is supported on your device.

---

## 📌 Notes

- Requires a secure HTTPS connection (`WiFiClientSecure`)
- OTA and debug features can be enabled via flags in `main.cpp`

---

## 🛠 Dependencies

- ESP8266 board package
- `WiFiManager`
- `ArduinoJson`
- `DallasTemperature`
- `OneWire`
- `ArduinoOTA`

---

## 📃 License

MIT License
