
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_BMP085.h>
#include <PubSubClient.h>
#include <atomic>
#include <math.h>

// ================= WIFI =================
const char* ssid = "Wokwi-GUEST";
const char* password = "";

// ================= MQTT =================
// Broker cong cong de mo phong Wokwi
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;

WiFiClient espClient;
PubSubClient mqtt(espClient);

Adafruit_BMP085 bmp;

// ================= GPIO =================
#define LED_PIN 18
#define BUZZER_PIN 19

// ================= NGUONG CANH BAO =================
#define TEMP_MIN 10.0
#define TEMP_MAX 40.0
#define ALT_LIMIT 5.0

// ================= CAU TRUC DU LIEU =================
struct SensorData {
  float temperature;
  float pressure;
  float altitude;
};

struct AlertData {
  float temperature;
  float altitude;
};

// ================= QUEUE =================
QueueHandle_t dataQueue;
QueueHandle_t dataAlert;

// Trang thai dieu khien thu cong
std::atomic<bool> manualLED(false);
std::atomic<bool> manualBuzzer(false);

// Trang thai canh bao tu dong
std::atomic<bool> alertLED(false);
std::atomic<bool> alertBuzzer(false);

// Cap nhat ngo ra
void updateOutputs() {
  digitalWrite(LED_PIN,
    manualLED.load() || alertLED.load());

  digitalWrite(BUZZER_PIN,
    manualBuzzer.load() || alertBuzzer.load());
}

// ================= MQTT CALLBACK =================
void mqttCallback(char* topic, byte* payload,
                  unsigned int length) {

  String message = "";

  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  message.trim();
  message.toUpperCase();

  if (strcmp(topic, "runner/control/led") == 0) {
    manualLED = (message == "ON");
  }

  if (strcmp(topic, "runner/control/buzzer") == 0) {
    manualBuzzer = (message == "ON");
  }

  updateOutputs();
}

// ================= KET NOI MQTT =================
void reconnectMQTT() {

  while (!mqtt.connected()) {

    Serial.println("Dang ket noi MQTT...");

    String clientID = "ESP32-Runner-" +
                      String((uint32_t)ESP.getEfuseMac(), HEX);

    if (mqtt.connect(clientID.c_str())) {

      Serial.println("MQTT connected!");

      mqtt.subscribe("runner/control/led");
      mqtt.subscribe("runner/control/buzzer");

    } else {

      Serial.print("MQTT error: ");
      Serial.println(mqtt.state());

      vTaskDelay(pdMS_TO_TICKS(2000));
    }
  }
}

// =================================================
// TASK 1: DOC DU LIEU CAM BIEN - CORE 1
// =================================================
void TaskReadData(void *pvParameters) {

  SensorData data;
  AlertData alert;

  while (1) {

    data.temperature = bmp.readTemperature();

    data.pressure = bmp.readPressure() / 100.0;

    data.altitude = bmp.readAltitude(101325);

    // Hien thi Serial
    Serial.println("===== RUNNER DATA =====");

    Serial.print("Nhiet do: ");
    Serial.print(data.temperature);
    Serial.println(" C");

    Serial.print("Ap suat: ");
    Serial.print(data.pressure);
    Serial.println(" hPa");

    Serial.print("Do cao: ");
    Serial.print(data.altitude);
    Serial.println(" m");

    // Gui du lieu day du vao dataQueue
    xQueueSend(dataQueue, &data, 0);

    // Gui du lieu canh bao vao dataAlert
    alert.temperature = data.temperature;
    alert.altitude = data.altitude;

    xQueueOverwrite(dataAlert, &alert);

    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// =================================================
// TASK 2: TRUYEN DATA LEN MQTT - CORE 0
// =================================================
void TaskTransmitData(void *pvParameters) {

  SensorData data;

  WiFi.mode(WIFI_STA);

  WiFi.begin(ssid, password);

  Serial.println("Dang ket noi WiFi...");

  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  Serial.println("WiFi connected!");
  Serial.println(WiFi.localIP());

  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setCallback(mqttCallback);

  while (1) {

    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (!mqtt.connected()) {
      reconnectMQTT();
    }

    mqtt.loop();

    // Nhan du lieu tu dataQueue
    if (xQueueReceive(dataQueue, &data,
                      pdMS_TO_TICKS(100)) == pdTRUE) {

      char payload[160];

      snprintf(payload, sizeof(payload),
        "{\"temperature\":%.2f,"
        "\"pressure\":%.2f,"
        "\"altitude\":%.2f}",
        data.temperature,
        data.pressure,
        data.altitude);

      mqtt.publish("runner/data", payload);

      Serial.println("Da gui du lieu len MQTT:");
      Serial.println(payload);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// =================================================
// TASK 3: CANH BAO - CORE 1
// =================================================
void TaskAlert(void *pvParameters) {

  AlertData alert;

  float previousAltitude = 0;

  bool firstReading = true;

  while (1) {

    if (xQueueReceive(dataAlert, &alert,
                      portMAX_DELAY) == pdTRUE) {

      bool temperatureWarning =
        (alert.temperature < TEMP_MIN ||
         alert.temperature > TEMP_MAX);

      bool altitudeWarning = false;

      // Kiem tra su thay doi do cao
      if (!firstReading) {

        float deltaAltitude =
          fabs(alert.altitude - previousAltitude);

        if (deltaAltitude > ALT_LIMIT) {
          altitudeWarning = true;
        }
      }

      previousAltitude = alert.altitude;
      firstReading = false;

      // Cap nhat canh bao tu dong
      bool warning =
        temperatureWarning || altitudeWarning;

      alertLED = warning;
      alertBuzzer = warning;

      updateOutputs();

      if (temperatureWarning) {
        Serial.println("CANH BAO: NHIET DO BAT THUONG!");
      }

      if (altitudeWarning) {
        Serial.println("CANH BAO: DO CAO THAY DOI DOT NGOT!");
      }

      if (!warning) {
        Serial.println("Trang thai: BINH THUONG");
      }
    }
  }
}

// ================= SETUP =================
void setup() {

  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  Wire.begin(21, 22);

  if (!bmp.begin()) {
    Serial.println("Khong tim thay BMP180!");
    while (1) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  // Tao Queue
  dataQueue = xQueueCreate(10, sizeof(SensorData));

  dataAlert = xQueueCreate(1, sizeof(AlertData));

  if (dataQueue == NULL || dataAlert == NULL) {
    Serial.println("Loi tao Queue!");
    while (1) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  // Tao Task va gan Core
  xTaskCreatePinnedToCore(
    TaskReadData,
    "ReadData",
    4096,
    NULL,
    2,
    NULL,
    1
  );

  xTaskCreatePinnedToCore(
    TaskTransmitData,
    "TransmitData",
    6144,
    NULL,
    1,
    NULL,
    0
  );

  xTaskCreatePinnedToCore(
    TaskAlert,
    "Alert",
    4096,
    NULL,
    2,
    NULL,
    1
  );
}

// ================= LOOP =================
void loop() {
  vTaskDelay(portMAX_DELAY);
}
