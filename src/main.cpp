#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <WiFi.h>
#include <HTTPClient.h> // ไลบรารีสำหรับยิง HTTP POST

// --- การตั้งค่า Hardware ---
#define I2C_SDA 21
#define I2C_SCL 22
#define BUZZER_PIN 12
#define MOTOR_PIN 14
const int MPU_ADDR = 0x68;

// --- ⚠️ การตั้งค่า WiFi (แก้ไขให้ตรงกับเน็ตบ้าน/มือถือของคุณ) ⚠️ ---
const char* ssid = "Just Me_2.4G";
const char* password = "Pokemongo12";

// URL สำหรับทดสอบรับข้อมูล (เดี๋ยวเราจะใช้เว็บจำลอง Backend กันก่อน)
const String serverURL = "https://webhook.site/a1b36095-0334-4447-a63c-0335552c7436"; 

// ตัวแปร Task
TaskHandle_t TaskSensorRead;
TaskHandle_t TaskAlert;
TaskHandle_t TaskNetwork; // Task ใหม่!

void setupMPU() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);  
  Wire.write(0);     
  Wire.endTransmission(true);
}

// 1. Task เซ็นเซอร์ (รันบน Core 1)
void sensorReadTask(void *pvParameters) {
  for(;;) {
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(0x3B); 
    Wire.endTransmission(false);
    Wire.requestFrom(MPU_ADDR, 6, true);  
    
    int16_t AcX = Wire.read()<<8 | Wire.read();  
    int16_t AcY = Wire.read()<<8 | Wire.read();  
    int16_t AcZ = Wire.read()<<8 | Wire.read();  

    float ax = AcX / 16384.0; float ay = AcY / 16384.0; float az = AcZ / 16384.0;
    float magnitude = sqrt(ax*ax + ay*ay + az*az);

    if (magnitude > 2.5 || magnitude < 0.4) {
      Serial.printf("\n⚠️ FALL DETECTED! แรงกระแทก: %.2f G\n", magnitude);
      
      // ปลุก Task Alert ให้ร้องเตือน!
      xTaskNotifyGive(TaskAlert); 
      // ปลุก Task Network ให้ส่งข้อมูลขึ้นเน็ต!
      xTaskNotifyGive(TaskNetwork); 
      
      vTaskDelay(4000 / portTICK_PERIOD_MS); 
    }
    vTaskDelay(50 / portTICK_PERIOD_MS); 
  }
}

// 2. Task แจ้งเตือน (รันบน Core 1)
void alertTask(void *pvParameters) {
  for(;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    for(int i=0; i<3; i++) {
      digitalWrite(BUZZER_PIN, HIGH); digitalWrite(MOTOR_PIN, HIGH);
      vTaskDelay(150 / portTICK_PERIOD_MS);
      digitalWrite(BUZZER_PIN, LOW); digitalWrite(MOTOR_PIN, LOW);
      vTaskDelay(150 / portTICK_PERIOD_MS);
    }
  }
}

// 3. Task เน็ตเวิร์ก (รันบน Core 0)
void networkTask(void *pvParameters) {
  // เชื่อมต่อ WiFi ครั้งแรกเมื่อเปิดเครื่อง
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(500 / portTICK_PERIOD_MS);
    Serial.print(".");
  }
  Serial.println("\n🌐 WiFi Connected!");
  Serial.print("IP Address: "); Serial.println(WiFi.localIP());

  for(;;) {
    // หลับรอจนกว่าเซ็นเซอร์จะปลุก (ประหยัดพลังงาน)
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if(WiFi.status() == WL_CONNECTED){
      HTTPClient http;
      http.begin(serverURL);
      http.addHeader("Content-Type", "application/json");

      // สร้างข้อมูล JSON (เตรียมพร้อมส่งเข้า MongoDB ในอนาคต)
      String jsonPayload = "{\"device_id\":\"ESP32_MVP_01\", \"status\":\"FALL_DETECTED\", \"magnitude\":\"HIGH\"}";

      Serial.println("🌐 กำลังส่งข้อมูลขึ้น Server...");
      int httpResponseCode = http.POST(jsonPayload); // ยิงข้อมูล!

      if (httpResponseCode > 0) {
        Serial.printf("✅ ส่งสำเร็จ! HTTP Response: %d\n", httpResponseCode);
      } else {
        Serial.printf("❌ ส่งไม่สำเร็จ! Error: %s\n", http.errorToString(httpResponseCode).c_str());
      }
      http.end();
    } else {
      Serial.println("❌ ไม่สามารถส่งได้ เพราะ WiFi หลุด");
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT); pinMode(MOTOR_PIN, OUTPUT);
  Wire.begin(I2C_SDA, I2C_SCL);
  setupMPU();
  
  // จัดสรร Core ในการรัน Task
  xTaskCreatePinnedToCore(sensorReadTask, "Sensor", 4096, NULL, 1, &TaskSensorRead, 1); // Core 1
  xTaskCreatePinnedToCore(alertTask, "Alert", 2048, NULL, 2, &TaskAlert, 1);        // Core 1
  
  // ให้อินเทอร์เน็ตไปรันที่ Core 0 เพื่อไม่ให้แย่งทรัพยากรเซ็นเซอร์
  xTaskCreatePinnedToCore(networkTask, "Network", 8192, NULL, 0, &TaskNetwork, 0);  // Core 0
}

void loop() {
  vTaskDelay(1000 / portTICK_PERIOD_MS);
}