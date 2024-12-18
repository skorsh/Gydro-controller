#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <EEPROM.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <TimeLib.h>
#include <ArduinoJson.h>
#include <ESP8266httpUpdate.h>

// Налаштування WiFi
const char* ssid = "TP-LINK_7BB2";
const char* password = "Hodapu99";
IPAddress staticIP(192, 168, 0, 173);
IPAddress gateway(192, 168, 0, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns1(8, 8, 8, 8);
IPAddress dns2(8, 8, 4, 4);

// Назва пристрою
const char* deviceName = "Gydro controller";

// Піни для реле
const int LIGHT_RELAY_PIN = D3;   // Реле освітлення
const int PUMP_RELAY_PIN = D4;    // Реле насосу
const int BACKUP_POWER_RELAY = D5;  // Реле резервного живлення
const int BACKUP_POWER_PUMP = D6;   // Реле насосу резервного живлення
const int POWER_MONITOR_PIN = A0;   // Пін для моніторингу напруги (A0)
#define I2C_SDA D2  // GPIO4
#define I2C_SCL D1  // GPIO5

// Можливі адреси BMP280
const byte BMP280_ADDRESSES[] = {0x76, 0x77};
const int NUM_ADDRESSES = sizeof(BMP280_ADDRESSES);
// Глобальні змінні
Adafruit_BMP280 bmp;
bool bmpFound = false;
byte bmpAddress = 0x00;

// Налаштування NTP
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 2 * 3600, 60000); // Київський час (+2 години)

// Веб-сервер
ESP8266WebServer server(80);

// Структура для налаштувань
struct Settings {
  // Освітлення
  int lightPeriod1Start;
  int lightPeriod1End;
  int lightPeriod2Start;
  int lightPeriod2End;

  // Насос (тепер в хвилинах)
  int pumpDayWorkDuration;
  int pumpDayStopDuration;
  int pumpNightWorkDuration;
  int pumpNightStopDuration;
};

// Глобальні змінні
Settings currentSettings;
bool lightStatus = false;
bool pumpStatus = false;
float temperature = 0.0;
float pressure = 0.0;
bool isBackupPower = false; // Змінна для зберігання стану живлення
bool previousPowerState = false; // Додано змінну для попереднього стану
bool voltageLogged = false; // Нова змінна для відстеження виводу напруги
// *** ОГОЛОШЕННЯ ЗМІННИХ ***
unsigned long lastPowerCheckTime = 0;
const unsigned long powerCheckInterval = 5000; // Перевірка кожні 5 секунд

void checkPowerStatus() {
  int analogValue = analogRead(POWER_MONITOR_PIN);
  float voltage = analogValue * (3.3 / 1023.0) * 3.63; // Коефіцієнт потрібно відкоригувати!

  // Виводимо напругу лише один раз при кожній перевірці
  if (!voltageLogged) {
    Serial.print("Напруга живлення: ");
    Serial.println(voltage);
    voltageLogged = true; // Встановлюємо флаг, що напруга вже виведена
  }

  isBackupPower = (voltage < 10.0); // Спрощений запис умови

  // Перевірка зміни стану живлення
  if (isBackupPower != previousPowerState) {
    if (isBackupPower) {
      Serial.println("Перехід на резервне живлення");
    } else {
      Serial.println("Робота від основного живлення");
    }
    previousPowerState = isBackupPower; // Оновлюємо попередній стан
  }

  setRelayState();
}

void setRelayState() {
  if (isBackupPower) {
    digitalWrite(BACKUP_POWER_RELAY, LOW);
    digitalWrite(PUMP_RELAY_PIN, LOW);
    digitalWrite(BACKUP_POWER_PUMP, HIGH);
  } else {
    digitalWrite(BACKUP_POWER_RELAY, HIGH);
    digitalWrite(PUMP_RELAY_PIN, HIGH);
    digitalWrite(BACKUP_POWER_PUMP, LOW);
  }
}

void controlPump() {
  int currentHour = timeClient.getHours();
  bool isDayPeriod = (currentHour >= 6 && currentHour < 22);

  static unsigned long pumpStartTime = 0;
  static unsigned long pumpStopTime = 0;
  static bool pumpRunning = false;

  unsigned long currentMillis = millis();

  // Множимо на 60 для отримання секунд
  int workDuration = (isDayPeriod ? currentSettings.pumpDayWorkDuration : currentSettings.pumpNightWorkDuration) * 60;
  int stopDuration = (isDayPeriod ? currentSettings.pumpDayStopDuration : currentSettings.pumpNightStopDuration) * 60;

  int pumpRelayPin = isBackupPower ? BACKUP_POWER_PUMP : PUMP_RELAY_PIN;

  if (!pumpRunning && (currentMillis - pumpStopTime >= stopDuration * 1000)) {
    digitalWrite(pumpRelayPin, HIGH);
    pumpRunning = true;
    pumpStartTime = currentMillis;
    pumpStatus = true;
    Serial.println("Насос увімкнено");
  }

  if (pumpRunning && (currentMillis - pumpStartTime >= workDuration * 1000)) {
    digitalWrite(pumpRelayPin, LOW);
    pumpRunning = false;
    pumpStopTime = currentMillis;
    pumpStatus = false;
    Serial.println("Насос вимкнено");
  }
}

void setupWiFi() {
  // Встановлення статичної IP-адреси
  WiFi.config(staticIP, gateway, subnet, dns1, dns2);
  
  // Встановлення імені пристрою
  WiFi.hostname(deviceName);
  
  // Підключення до WiFi
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nНе вдалося підключитися до WiFi. Перезавантаження.");
    ESP.restart();
  }
  
  Serial.println("\nПідключено до WiFi");
  Serial.print("IP-адреса: ");
  Serial.println(WiFi.localIP());
}

void diagnoseI2C() {
  Serial.println("\n=== Діагностика I2C ===");
  
  // Перевірка підключення ліній SDA та SCL
  pinMode(I2C_SDA, INPUT_PULLUP);
  pinMode(I2C_SCL, INPUT_PULLUP);
  
  Serial.print("Стан лінії SDA (D2): ");
  Serial.println(digitalRead(I2C_SDA) == HIGH ? "HIGH (підтягнута)" : "LOW");
  
  Serial.print("Стан лінії SCL (D1): ");
  Serial.println(digitalRead(I2C_SCL) == HIGH ? "HIGH (підтягнута)" : "LOW");

  // Сканування I2C пристроїв
  Serial.println("\nСканування I2C пристроїв:");
  
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);  // Стандартна швидкість I2C
  
  int nDevices = 0;
  for(byte address = 1; address < 127; address++ ) {
    Wire.beginTransmission(address);
    byte error = Wire.endTransmission();
    
    if (error == 0) {
      Serial.print("I2C пристрій знайдено за адресою: 0x");
      if (address < 16) Serial.print("0");
      Serial.println(address, HEX);
      nDevices++;
    }
    else if (error == 4) {
      Serial.print("Невідома помилка при адресі: 0x");
      if (address < 16) Serial.print("0");
      Serial.println(address, HEX);
    }    
  }
  
  if (nDevices == 0)
    Serial.println("Жодного I2C пристрою не знайдено!");
  else
    Serial.printf("Знайдено %d I2C пристроїв\n", nDevices);
}

bool initBMP280() {
  Serial.println("\n=== Ініціалізація BMP280 ===");
  
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);  // Стандартна швидкість I2C
  delay(100);             // Затримка перед ініціалізацією

  for (int i = 0; i < NUM_ADDRESSES; i++) {
    if (bmp.begin(BMP280_ADDRESSES[i])) {
      bmpFound = true;
      bmpAddress = BMP280_ADDRESSES[i];
      Serial.printf("BMP280 знайдено за адресою: 0x%02X\n", bmpAddress);
      return true;
    }
  }

  Serial.println("Не вдалося ініціалізувати BMP280. Перевірте підключення.");
  return false;
}
  
void updateSensorData() {
  static unsigned long lastSensorUpdate = 0;
  static byte errorCount = 0;

  if (millis() - lastSensorUpdate >= 60000) {
    float tempRead = bmp.readTemperature();
    float pressRead = bmp.readPressure() / 100.0;

    if (tempRead > -50 && tempRead < 100 &&
        pressRead > 800 && pressRead < 1200) {
      temperature = tempRead;
      pressure = pressRead;

      Serial.print("Оновлено дані: ");
      Serial.print(temperature, 1);
      Serial.print("°C, ");
      Serial.print(pressure, 1);
      Serial.println(" гПа");

      errorCount = 0;
    } else {
      Serial.println("Некоректні дані від датчика");
      errorCount++;
    }

    if (errorCount > 5) {
      Serial.println("Критична помилка датчика. Перезапуск.");
      ESP.restart();
    }

    lastSensorUpdate = millis();
  }
}

void controlLighting() {
  // Поточний час
  int currentHour = timeClient.getHours();
  int currentMinute = timeClient.getMinutes();
  
  // Перетворення часу в хвилини для простішого порівняння
  int currentTimeInMinutes = currentHour * 60 + currentMinute;
  
  // Перевірка першого періоду освітлення
  bool inFirstLightPeriod = 
    currentTimeInMinutes >= currentSettings.lightPeriod1Start &&
    currentTimeInMinutes < currentSettings.lightPeriod1End;
  
  // Перевірка другого періоду освітлення
  bool inSecondLightPeriod = 
    currentTimeInMinutes >= currentSettings.lightPeriod2Start &&
    currentTimeInMinutes < currentSettings.lightPeriod2End;
  
  // Встановлення статусу реле освітлення
  lightStatus = inFirstLightPeriod || inSecondLightPeriod;
  digitalWrite(LIGHT_RELAY_PIN, lightStatus ? HIGH : LOW);
  
  // Логування змін
  static bool lastLightStatus = !lightStatus;
  if (lightStatus != lastLightStatus) {
    Serial.print("Статус освітлення: ");
    Serial.println(lightStatus ? "Увімкнено" : "Вимкнено");
    lastLightStatus = lightStatus;
  }
}

void loadSettingsFromEEPROM() {
  // Завантаження налаштувань з EEPROM
  EEPROM.get(0, currentSettings);

  // Перевірка коректності налаштувань (додано перевірку pumpDayWorkDuration)
  if (currentSettings.lightPeriod1Start < 0 || currentSettings.lightPeriod1Start > 1440 ||
      currentSettings.pumpDayWorkDuration <= 0) { // Нова умова

    // Встановлення тестових значень за замовчуванням (в хвилинах!)
    currentSettings.lightPeriod1Start = 360;  // 6:00
    currentSettings.lightPeriod1End = 720;    // 12:00
    currentSettings.lightPeriod2Start = 1080; // 18:00
    currentSettings.lightPeriod2End = 1440;   // 24:00

    currentSettings.pumpDayWorkDuration = 5;    // 5 хвилин
    currentSettings.pumpDayStopDuration = 10;   // 10 хвилин
    currentSettings.pumpNightWorkDuration = 3;  // 3 хвилини
    currentSettings.pumpNightStopDuration = 15; // 15 хвилин

    // Збереження тестових налаштувань
    EEPROM.put(0, currentSettings);
    EEPROM.commit();

    Serial.println("Завантажено тестові налаштування");
  }

  Serial.println("Налаштування успішно завантажено з EEPROM");
}

void setupWebServer() {
  // Налаштування маршрутів
  server.on("/", handleRoot);
  server.on("/getconfig", handleGetConfig);
  server.on("/saveconfig", HTTP_POST, handleSaveConfig);
  
  // Запуск сервера
  server.begin();
  Serial.println("HTTP сервер запущено");
}

void handleRoot() {
  String html = R"(
<!DOCTYPE html>
<html lang='uk'>
<head>
    <meta charset='UTF-8'>
    <title>Gydro Controller</title>
    <meta name='viewport' content='width=device-width, initial-scale=1'>
    <style>
        body { font-family: Arial, sans-serif; max-width: 600px; margin: 0 auto; padding: 20px; }
        .block { border: 1px solid #ddd; padding: 15px; margin-bottom: 15px; }
        input, select { width: 100%; margin: 5px 0; }
        button { width: 100%; padding: 10px; }
    </style>
</head>
<body>
    <h1>Gydro Controller</h1>
    
    <div class='block' id='systemInfo'>
        <h2>Інформація про систему</h2>
        <p>Дата: <span id='currentDate'>-</span></p>
        <p>Час: <span id='currentTime'>-</span></p>
        <p>Температура: <span id='temperature'>-</span>°C</p>
        <p>Тиск: <span id='pressure'>-</span> гПа</p>
        <p>Статус освітлення: <span id='lightStatus'>-</span></p>
        <p>Статус насосу: <span id='pumpStatus'>-</span></p>
    </div>

    <div class='block'>
        <h2>Керування освітленням</h2>
        <div>
            <label>Перший період</label>
            <input type='time' id='lightPeriod1Start'>
            <input type='time' id='lightPeriod1End'>
        </div>
        <div>
            <label>Другий період</label>
            <input type='time' id='lightPeriod2Start'>
            <input type='time' id='lightPeriod2End'>
        </div>
    </div>

    <div class='block'>
  <h2>Керування насосом</h2>
  <div>
    <label>Денний період (робота/пауза) хв</label>
    <input type='number' id='pumpDayWorkDuration' placeholder='Тривалість роботи (хв)'>
    <input type='number' id='pumpDayStopDuration' placeholder='Тривалість паузи (хв)'>
  </div>
  <div>
    <label>Нічний період (робота/пауза) хв</label>
    <input type='number' id='pumpNightWorkDuration' placeholder='Тривалість роботи (хв)'>
    <input type='number' id='pumpNightStopDuration' placeholder='Тривалість паузи (хв)'>
  </div>
</div>

    <button onclick='saveConfig()'>Зберегти налаштування</button>

    <div id='saveResult'></div>

    <script>
        function loadConfig() {
            fetch('/getconfig')
                .then(response => response.json())
                .then(data => {
                    // Налаштування освітлення
                    document.getElementById('lightPeriod1Start').value = minutesToTime(data.lightPeriod1Start);
                    document.getElementById('lightPeriod1End').value = minutesToTime(data.lightPeriod1End);
                    document.getElementById('lightPeriod2Start').value = minutesToTime(data.lightPeriod2Start);
                    document.getElementById('lightPeriod2End').value = minutesToTime(data.lightPeriod2End);

                    // Налаштування насосу
                    document.getElementById('pumpDayWorkDuration').value = data.pumpDayWorkDuration;
                    document.getElementById('pumpDayStopDuration').value = data.pumpDayStopDuration;
                    document.getElementById('pumpNightWorkDuration').value = data.pumpNightWorkDuration;
                    document.getElementById('pumpNightStopDuration').value = data.pumpNightStopDuration;
                })
                .catch(error => {
                    console.error('Помилка завантаження:', error);
                });

            // Оновлення системної інформації
            updateSystemInfo();
        }

        function minutesToTime(minutes) {
            const hours = Math.floor(minutes / 60);
            const mins = minutes % 60;
            return `${hours.toString().padStart(2, '0')}:${mins.toString().padStart(2, '0')}`;
        }

        function timeToMinutes(time) {
            const [hours, minutes] = time.split(':').map(Number);
            return hours * 60 + minutes;
        }

        function updateSystemInfo() {
            fetch('/getconfig')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('temperature').textContent = data.temperature.toFixed(1);
                    document.getElementById('pressure').textContent = data.pressure.toFixed(1);
                    document.getElementById('lightStatus').textContent = data.lightStatus ? 'Увімкнено' : 'Вимкнено';
                    document.getElementById('pumpStatus').textContent = data.pumpStatus ? 'Працює' : 'Зупинено';

                    const now = new Date();
                    document.getElementById('currentDate').textContent = now.toLocaleDateString('uk-UA');
                    document.getElementById('currentTime').textContent = now.toLocaleTimeString('uk-UA');
                });
        }

        function saveConfig() {
            const config = {
                lightPeriod1Start: timeToMinutes(document.getElementById('lightPeriod1Start').value),
                lightPeriod1End: timeToMinutes(document.getElementById('lightPeriod1End').value),
                lightPeriod2Start: timeToMinutes(document.getElementById('lightPeriod2Start').value),
                lightPeriod2End: timeToMinutes(document.getElementById('lightPeriod2End').value),
                pumpDayWorkDuration: Number(document.getElementById('pumpDayWorkDuration').value),
                pumpDayStopDuration: Number(document.getElementById('pumpDayStopDuration').value),
                pumpNightWorkDuration: Number(document.getElementById('pumpNightWorkDuration').value),
                pumpNightStopDuration: Number(document.getElementById('pumpNightStopDuration').value)
            };

            fetch('/saveconfig', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(config)
            })
            .then(response => response.json())
            .then(data => {
                const resultDiv = document.getElementById('saveResult');
                resultDiv.textContent = data.status;
                resultDiv.style.color = data.status === 'Налаштування збережено' ? 'green' : 'red';
                
                // Перезавантаження конфігурації після успішного збереження
                loadConfig();
            })
            .catch(error => {
                console.error('Помилка:', error);
            });
        }

        // Завантаження конфігурації при завантаженні сторінки
        window.onload = loadConfig;
        
        // Оновлення інформації кожні 60 секунд
        setInterval(updateSystemInfo, 60000);
    </script>
</body>
</html>
)";

  server.send(200, "text/html", html);
}

void handleGetConfig() {
  StaticJsonDocument<1024> jsonDoc;
  
    // Системна інформація
  if (isnan(temperature) || isinf(temperature)) {
    jsonDoc["temperature"] = "Помилка зчитування"; // Або інше повідомлення
  } else {
    jsonDoc["temperature"] = temperature;
  }

  if (isnan(pressure) || isinf(pressure)) {
    jsonDoc["pressure"] = "Помилка зчитування"; // Або інше повідомлення
  } else {
    jsonDoc["pressure"] = pressure;
  }

  // Налаштування освітлення
  jsonDoc["lightPeriod1Start"] = currentSettings.lightPeriod1Start;
  jsonDoc["lightPeriod1End"] = currentSettings.lightPeriod1End;
  jsonDoc["lightPeriod2Start"] = currentSettings.lightPeriod2Start;
  jsonDoc["lightPeriod2End"] = currentSettings.lightPeriod2End;

  // Налаштування насосу
  jsonDoc["pumpDayWorkDuration"] = currentSettings.pumpDayWorkDuration;
  jsonDoc["pumpDayStopDuration"] = currentSettings.pumpDayStopDuration;
  jsonDoc["pumpNightWorkDuration"] = currentSettings.pumpNightWorkDuration;
  jsonDoc["pumpNightStopDuration"] = currentSettings.pumpNightStopDuration;

  // Поточний час
  jsonDoc["currentTime"] = timeClient.getFormattedTime();

  String jsonResponse;
  serializeJson(jsonDoc, jsonResponse);
  server.send(200, "application/json", jsonResponse);
}

void handleSaveConfig() {
  StaticJsonDocument<512> jsonDoc;
  DeserializationError error = deserializeJson(jsonDoc, server.arg("plain"));

  if (error) {
    server.send(400, "application/json", "{\"status\":\"Некоректні дані\"}");
    return;
  }

  // Перевірка та збереження налаштувань освітлення
  currentSettings.lightPeriod1Start = jsonDoc["lightPeriod1Start"].as<int>();
  currentSettings.lightPeriod1End = jsonDoc["lightPeriod1End"].as<int>();
  currentSettings.lightPeriod2Start = jsonDoc["lightPeriod2Start"].as<int>();
  currentSettings.lightPeriod2End = jsonDoc["lightPeriod2End"].as<int>();

  // Перевірка та збереження налаштувань насосу
  currentSettings.pumpDayWorkDuration = jsonDoc["pumpDayWorkDuration"].as<int>();
  currentSettings.pumpDayStopDuration = jsonDoc["pumpDayStopDuration"].as<int>();
  currentSettings.pumpNightWorkDuration = jsonDoc["pumpNightWorkDuration"].as<int>();
  currentSettings.pumpNightStopDuration = jsonDoc["pumpNightStopDuration"].as<int>();

  // Збереження в EEPROM
  EEPROM.put(0, currentSettings);
  EEPROM.commit();

  // Логування змін
  Serial.println("Нові налаштування збережено в EEPROM");

  server.send(200, "application/json", "{\"status\":\"Налаштування збережено\"}");
}



void setup() {
  Serial.begin(115200);
  
  // Ініціалізація EEPROM
  EEPROM.begin(512);
  
  // Налаштування пінів реле
  pinMode(LIGHT_RELAY_PIN, OUTPUT);
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  pinMode(BACKUP_POWER_RELAY, OUTPUT);
  pinMode(BACKUP_POWER_PUMP, OUTPUT);

  // Ініціалізація моніторингу живлення
  pinMode(POWER_MONITOR_PIN, INPUT);

  checkPowerStatus(); // Перевірка стану живлення при запуску

  diagnoseI2C();

  if (!initBMP280()) {
    Serial.println("Помилка ініціалізації BMP280. Перезавантаження.");
    delay(5000);
    ESP.restart();
  }

   // Діагностичний код BMP280
  if (bmpFound) {
    float temp = bmp.readTemperature();
    float pres = bmp.readPressure() / 100.0;
    
    Serial.print("Температура: ");
    Serial.print(temp);
    Serial.print("°C, Тиск: ");
    Serial.print(pres);
    Serial.println(" гПа");
  }
  
  // Підключення до WiFi
  setupWiFi();
  
  // Ініціалізація NTP
  timeClient.begin();
  
  // Завантаження налаштувань
  loadSettingsFromEEPROM();
  
  // Налаштування веб-сервера
  setupWebServer();
}

void loop() {
  server.handleClient();
  timeClient.update();

if (millis() - lastPowerCheckTime >= powerCheckInterval) {
    checkPowerStatus();
    lastPowerCheckTime = millis();
  }
  
  // Оновлення параметрів
  updateSensorData();
  controlLighting();
  controlPump();

}
