
// === Libs ===
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <VL53L1X.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <PubSubClient.h>
#include <algorithm>

// === PIN-Conf ===
#define ONE_WIRE_BUS 4

// === Global Vars ===
AsyncWebServer server(80);
DNSServer dnsServer;
VL53L1X vl53;
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
WiFiClient espClient;
PubSubClient mqttClient(espClient);
Preferences preferences;

bool isInApMode = false;

struct Config {
  char wifi_ssid[32];
  char wifi_password[64];
  char mqtt_server[64];
  int mqtt_port;
  char mqtt_user[32];
  char mqtt_password[64];
  struct CalibPoint {
    int distance;
    int liter;
  } calibPoints[5];
};
Config config;

float currentTemperature = -127.0;
int currentDistance = -1;
float currentLiters = -1.0;
unsigned long lastMeasurementTime = 0;
const long measurementInterval = 5000;

// === HTML-Generator ===
String getConfigPage() {
    String html = "<html><head><title>Infinitys Level Sensor</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>body{font-family: Arial, sans-serif; background-color: #f4f4f4; margin: 20px;}";
    html += "div{background: #fff; border: 1px solid #ddd; padding: 20px; border-radius: 5px; margin-bottom: 20px;}";
    html += "h1,h2{color: #333; text-align: center;}";
    html += "h2{border-bottom: 2px solid #007bff; padding-bottom: 10px;}";
    html += "input[type='text'], input[type='password'], input[type='number']{width: 100%; padding: 8px; margin: 10px 0; box-sizing: border-box; border: 1px solid #ccc; border-radius: 4px;}";
    html += "input[type='submit'], button{background-color: #007bff; color: white; padding: 14px 20px; margin: 8px 0; border: none; border-radius: 4px; cursor: pointer; width: 100%; font-size: 16px;}";
    html += "input[type='submit']:hover, button:hover{background-color: #0056b3;} .calib-grid{display: grid; grid-template-columns: 1fr 1fr; gap: 10px;} a{text-decoration: none;}</style></head>";
    html += "<body><h1>Level Sensor</h1>";
    html += "<form action='/save' method='POST'>";
    html += "<div><h2>WiFi Settings</h2>";
    html += "SSID: <input type='text' name='wifi_ssid' placeholder='SSID' value='" + String(config.wifi_ssid) + "' required><br>";
    html += "Password: <input type='password' name='wifi_password' placeholder='WiFi Password'><br></div>";
    html += "<div><h2>MQTT-Broker</h2>";
    html += "Server IP/Hostname: <input type='text' name='mqtt_server' value='" + String(config.mqtt_server) + "' required><br>";
    html += "Port: <input type='number' name='mqtt_port' value='" + String(config.mqtt_port) + "' required><br>";
    html += "User (optional): <input type='text' name='mqtt_user' value='" + String(config.mqtt_user) + "'><br>";
    html += "Password (optional): <input type='password' name='mqtt_password'><br></div>";
    html += "<div><h2>Calibration</h2>";
    html += "<p>Input distance (sensor to water) in <strong>Millimeters</strong> and the fill volume in <strong>Liters</strong> . Order from 'full' (smalest distance) to 'empty' (greatest distance).</p>";
    html += "<div class='calib-grid'>";
    for (int i = 0; i < 5; i++) {
      html += "<div><b>Point " + String(i+1) + "</b><br>";
      html += "Distance (mm): <input type='number' name='dist" + String(i) + "' value='" + String(config.calibPoints[i].distance) + "' required><br>";
      html += "Liters: <input type='number' name='liter" + String(i) + "' value='" + String(config.calibPoints[i].liter) + "' required><br></div>";
    }
    html += "</div></div>";
    html += "<input type='submit' value='Save & Restart'>";
    html += "</form>";
    html += "<a href='/'><button style='background-color: #6c757d;'>Back to the Dashboard</button>";
    html += "</body></html>";
    return html;
}
String getDashboardPage() {
    String html = "<html><head><title>Level Sensor</title>";
    html += "<meta http-equiv='refresh' content='5'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>body{font-family: Arial, sans-serif; text-align: center; background-color: #f0f0f0; padding-top: 20px;}";
    html += ".card{background: white; margin: 15px auto; padding: 20px; border-radius: 8px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); display: inline-block; min-width: 200px;}";
    html += "h1{color: #333;} h2{color: #007bff; font-size: 2.5em; margin: 10px 0;} .unit{font-size: 0.8em; color: #666;}";
    html += "a{text-decoration: none;} button{background-color: #28a745; color: white; padding: 15px 30px; border: none; border-radius: 5px; cursor: pointer; font-size: 1em; margin-top: 20px;}";
    html += "button:hover{background-color: #218838;}</style></head>";
    html += "<body><h1>Level</h1>";
    html += "<div class='card'><h2>" + String(currentLiters, 1) + "<span class='unit'> Liter</span></h2>Level</div>";
    html += "<div class='card'><h2>" + String(currentDistance) + "<span class='unit'> mm</span></h2>Distance</div>";
    html += "<div class='card'><h2>" + String(currentTemperature, 1) + "<span class='unit'> &deg;C</span></h2>Temperature</div>";
    html += "<br><a href='/config'><button>Config & Calibration</button></a>";
    html += "</body></html>";
    return html;
}

// === sensors-, cals- & MQTT ===
void readSensors() {
  const int numReadings = 10;
  const int outlierMargin = 100;
  uint16_t readings[numReadings];
  int validReadingsCount = 0;

  for (int i = 0; i < numReadings; i++) {
    readings[i] = vl53.read();
    if (!vl53.timeoutOccurred()) {
      validReadingsCount++;
    }
    delay(20);
  }
  
  if (validReadingsCount < 3) {
      Serial.println("Not enough valid measurements!");
      currentDistance = -1;
  } else {
    std::sort(readings, readings + numReadings);
    uint16_t median = readings[numReadings / 2];
    long total_sum = 0;
    int good_readings_count = 0;
    for (int i = 0; i < numReadings; i++) {
      if (abs(readings[i] - median) <= outlierMargin) {
        total_sum += readings[i];
        good_readings_count++;
      }
    }
    if (good_readings_count > 0) {
      currentDistance = total_sum / good_readings_count;
      Serial.printf("Filtered distance: %d mm (from %d valid measurements)\n", currentDistance, good_readings_count);
    } else {
      Serial.println("All measurements invalid!");
      currentDistance = -1;
    }
  }

  sensors.requestTemperatures();
  float temp = sensors.getTempCByIndex(0);
  if (temp != DEVICE_DISCONNECTED_C) {
    currentTemperature = temp;
  } else {
    currentTemperature = -127.0;
  }
}
float calculateLiters(int distance) {
  if (distance < 0) return -1.0;
  if (distance <= config.calibPoints[0].distance) return config.calibPoints[0].liter;
  if (distance >= config.calibPoints[4].distance) return config.calibPoints[4].liter;
  for (int i = 0; i < 4; i++) {
    if (distance >= config.calibPoints[i].distance && distance < config.calibPoints[i+1].distance) {
      long d1 = config.calibPoints[i].distance, l1 = config.calibPoints[i].liter;
      long d2 = config.calibPoints[i+1].distance, l2 = config.calibPoints[i+1].liter;
      if (d2 - d1 == 0) return l1;
      return l1 + (float)(distance - d1) * (l2 - l1) / (float)(d2 - d1);
    }
  }
  return -1.0;
}
void reconnectMQTT() {
  if(isInApMode) return;
  while (!mqttClient.connected()) {
    Serial.print("Trying MQTT Connection...");
    String clientId = "Level-Sense-";
    clientId += String(random(0xffff), HEX);
    if (mqttClient.connect(clientId.c_str(), config.mqtt_user, config.mqtt_password)) {
      Serial.println("connected!");
    } else {
      Serial.print("error, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" retry in 5 seconds");
      delay(5000);
    }
  }
}
void publishMqttData() {
  if (!mqttClient.connected()) return;
  char payload[10];
  dtostrf(currentDistance, 4, 0, payload);
  mqttClient.publish("fuellstand/distance_mm", payload);
  dtostrf(currentTemperature, 4, 1, payload);
  mqttClient.publish("fuellstand/temperature_c", payload);
  dtostrf(currentLiters, 5, 1, payload);
  mqttClient.publish("fuellstand/liters", payload);
}

// === SETUP ===
void setup() {
  Serial.begin(115200);
  Serial.println("\n\nLevel Sensor (fully) started...");

  Wire.begin();
  vl53.setTimeout(500);
  if (!vl53.init()) {
    Serial.println(F("Initialisation error VL53L1X Sensor"));
  } else {
    vl53.setROISize(4, 4);
    vl53.setMeasurementTimingBudget(50000);
    vl53.startContinuous(50);
  }
  sensors.begin();
  
  memset(&config, 0, sizeof(Config));
  config.mqtt_port = 1883;
  preferences.begin("config", true);
  preferences.getBytes("settings", &config, sizeof(Config));
  preferences.end();
  
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", getDashboardPage());
  });
  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", getConfigPage());
  });
  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request){
    strncpy(config.wifi_ssid, request->arg("wifi_ssid").c_str(), sizeof(config.wifi_ssid));
    strncpy(config.wifi_password, request->arg("wifi_password").c_str(), sizeof(config.wifi_password));
    strncpy(config.mqtt_server, request->arg("mqtt_server").c_str(), sizeof(config.mqtt_server));
    config.mqtt_port = request->arg("mqtt_port").toInt();
    strncpy(config.mqtt_user, request->arg("mqtt_user").c_str(), sizeof(config.mqtt_user));
    strncpy(config.mqtt_password, request->arg("mqtt_password").c_str(), sizeof(config.mqtt_password));
    for (int i = 0; i < 5; i++) {
        config.calibPoints[i].distance = request->arg("dist" + String(i)).toInt();
        config.calibPoints[i].liter = request->arg("liter" + String(i)).toInt();
    }
    preferences.begin("config", false);
    preferences.putBytes("settings", &config, sizeof(Config));
    preferences.end();
    String html = "<html><head><meta http-equiv='refresh' content='3;url=/' /></head><body><h1>Gespeichert!</h1><p>Sensor is restarting...</p></body></html>";
    request->send(200, "text/html", html);
    delay(3000);
    ESP.restart();
  });
  server.onNotFound([](AsyncWebServerRequest *request) {
    request->redirect("/");
  });

  if (strlen(config.wifi_ssid) == 0) {
    Serial.println("WiFi not Configured. Starting in Access-Point mode...");
    isInApMode = true;
  } else {
    WiFi.mode(WIFI_STA);
    WiFi.begin(config.wifi_ssid, config.wifi_password);
    Serial.print("Connecting so known WiFi: "); Serial.println(config.wifi_ssid);
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 20000) {
      Serial.print(".");
      delay(500);
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nConnected! IP-Adress: ");
      Serial.println(WiFi.localIP());
      isInApMode = false;
      mqttClient.setServer(config.mqtt_server, config.mqtt_port);
    } else {
      Serial.println("\nConnection failed. Starting Fallback Access-Point...");
      WiFi.mode(WIFI_AP);
      isInApMode = true;
    }
  }

  if (isInApMode) {
    WiFi.softAP("Infinity-level-sense");
    IPAddress apIP = WiFi.softAPIP();
    Serial.print("AP IP: "); Serial.println(apIP);
    dnsServer.start(53, "*", apIP);
  }
  
  server.begin();
}

// === LOOP ===
void loop() {
  if (isInApMode) {
    dnsServer.processNextRequest();
  } else {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi connection lost. retrying...");
      mqttClient.disconnect();
      
      for (int i = 1; i <= 5; i++) {
        Serial.printf("reconnecting for %d/5...\n time", i);
        WiFi.reconnect();
        
        unsigned long startAttemptTime = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
          delay(500);
          Serial.print(".");
        }

        if (WiFi.status() == WL_CONNECTED) {
          Serial.println("\nConnection reastablished!");
          break;
        }
        
        if (i == 5) {
          Serial.println("\nCould not connect to WiFi. Restarting...");
          delay(1000);
          ESP.restart();
        }
      }
    }

    if (!mqttClient.connected()) {
      reconnectMQTT();
    }
    mqttClient.loop();
  }

  if (millis() - lastMeasurementTime > measurementInterval) {
    readSensors();
    if(currentDistance >= 0) {
      currentLiters = calculateLiters(currentDistance);
    }
    
    if (!isInApMode && WiFi.status() == WL_CONNECTED && mqttClient.connected()) {
      publishMqttData();
    }
    lastMeasurementTime = millis();
  }
}