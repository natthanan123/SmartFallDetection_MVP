#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Natthanan123-project-1_inferencing.h> 

const char* ssid = "MM";
const char* password = "12345678";
const String serverName = "https://smart-fall-dashboard.vercel.app/api/events";
const String deviceID = "ESP32_AI_MVP";
const int MPU_ADDR = 0x68;

#define BUZZER_PIN 12
#define MOTOR_PIN 14

TaskHandle_t SensorTaskHandle;
TaskHandle_t AITaskHandle;
TaskHandle_t WiFiTaskHandle;
TaskHandle_t AlertTaskHandle; 

QueueHandle_t sensorQueue;
SemaphoreHandle_t fallSemaphore;
SemaphoreHandle_t serialMutex;

struct SensorData {
  float ax;
  float ay;
  float az;
  float magnitude;
};

float currentMaxMagnitude = 0.0;

void setupMPU() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B); Wire.write(0); Wire.endTransmission(true);
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x1C); Wire.write(0x08); Wire.endTransmission(true);
}

void connectWiFi() {
  xSemaphoreTake(serialMutex, portMAX_DELAY);
  Serial.print("Connecting to WiFi...");
  xSemaphoreGive(serialMutex);
  
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  
  xSemaphoreTake(serialMutex, portMAX_DELAY);
  Serial.println("\nWiFi Connected!");
  xSemaphoreGive(serialMutex);
}


void vSensorTask(void *pvParameters) {
  SensorData data;
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xFrequency = pdMS_TO_TICKS(20);

  for (;;) {
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(0x3B); Wire.endTransmission(false);
    Wire.requestFrom(MPU_ADDR, 6, true);  
    
    int16_t AcX = Wire.read()<<8 | Wire.read();  
    int16_t AcY = Wire.read()<<8 | Wire.read();  
    int16_t AcZ = Wire.read()<<8 | Wire.read();  

    data.ax = AcX / 8192.0;
    data.ay = AcY / 8192.0;
    data.az = AcZ / 8192.0;
    data.magnitude = sqrt(data.ax*data.ax + data.ay*data.ay + data.az*data.az);

    xQueueSend(sensorQueue, &data, 0);
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
  }
}

void vAITask(void *pvParameters) {
  SensorData data;
  float features[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];
  int feature_ix = 0;
  float maxMagInWindow = 0.0;

  for (;;) {
    if (xQueueReceive(sensorQueue, &data, portMAX_DELAY) == pdPASS) {
      features[feature_ix++] = data.ax;
      features[feature_ix++] = data.ay;
      features[feature_ix++] = data.az;
      
      if (data.magnitude > maxMagInWindow) {
        maxMagInWindow = data.magnitude;
      }

      if (feature_ix >= EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE) {
        feature_ix = 0; 

        signal_t signal;
        numpy::signal_from_buffer(features, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal);
        
        ei_impulse_result_t result = { 0 };
        run_classifier(&signal, &result, false);

        for (uint16_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
          String label = String(result.classification[i].label);
          float value = result.classification[i].value;

          if (label == "fall" && value >= 0.80) {
            xSemaphoreTake(serialMutex, portMAX_DELAY);
            Serial.print("FALL DETECTED! Conf: "); Serial.print(value * 100); Serial.println("%");
            xSemaphoreGive(serialMutex);

            currentMaxMagnitude = maxMagInWindow;
            
            xSemaphoreGive(fallSemaphore);
            xTaskNotifyGive(AlertTaskHandle);
          }
        }
        maxMagInWindow = 0.0;
      }
    }
  }
}


void vAlertTask(void *pvParameters) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    xSemaphoreTake(serialMutex, portMAX_DELAY);
    Serial.println("🔊 Alert Task ตื่นแล้ว! สั่งเปิด Buzzer และ Motor!");
    xSemaphoreGive(serialMutex);
    
    for(int i = 0; i < 10; i++) {
      digitalWrite(BUZZER_PIN, HIGH);
      digitalWrite(MOTOR_PIN, HIGH);
      vTaskDelay(150 / portTICK_PERIOD_MS); 
      
      digitalWrite(BUZZER_PIN, LOW);
      digitalWrite(MOTOR_PIN, LOW);
      vTaskDelay(150 / portTICK_PERIOD_MS);
    }
  }
}


void vWiFiTask(void *pvParameters) {
  const TickType_t cooldownTime = pdMS_TO_TICKS(10000);

  for (;;) {
    if (xSemaphoreTake(fallSemaphore, portMAX_DELAY) == pdPASS) {
      
      if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        http.begin(serverName);
        http.addHeader("Content-Type", "application/json");

        String jsonPayload = "{\"device_id\":\"" + deviceID + "\",";
        jsonPayload += "\"magnitude\":" + String(currentMaxMagnitude, 2) + ",";
        jsonPayload += "\"status\":\"UNACKNOWLEDGED\"}";

        int httpResponseCode = http.POST(jsonPayload);
        
        xSemaphoreTake(serialMutex, portMAX_DELAY);
        if (httpResponseCode > 0) {
          Serial.print("Web Alert Sent! Code: "); Serial.println(httpResponseCode);
        } else {
          Serial.print("Send Error: "); Serial.println(httpResponseCode);
        }
        xSemaphoreGive(serialMutex);
        http.end();
      } else {
        connectWiFi();
      }

      vTaskDelay(cooldownTime);
      xSemaphoreTake(fallSemaphore, 0); 
    }
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(MOTOR_PIN, LOW);

  setupMPU();
  
  serialMutex = xSemaphoreCreateMutex();
  fallSemaphore = xSemaphoreCreateBinary();
  sensorQueue = xQueueCreate(10, sizeof(SensorData));

  connectWiFi();

  xTaskCreatePinnedToCore(vSensorTask, "Sensor Task", 4096, NULL, 3, &SensorTaskHandle, 1);
  xTaskCreatePinnedToCore(vAITask,     "AI Task",     8192, NULL, 2, &AITaskHandle,     1);
  xTaskCreatePinnedToCore(vWiFiTask,   "WiFi Task",   8192, NULL, 1, &WiFiTaskHandle,   0);
  xTaskCreatePinnedToCore(vAlertTask,  "Alert Task",  2048, NULL, 4, &AlertTaskHandle,  0);

  xSemaphoreTake(serialMutex, portMAX_DELAY);
  Serial.println("FreeRTOS Master Scheduler Started! Ready for AI Fall Detection.");
  xSemaphoreGive(serialMutex);
}

void loop() {
  vTaskDelete(NULL); 
}