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
