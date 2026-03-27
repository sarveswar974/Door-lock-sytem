#include <Keypad.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ============ WiFi Configuration ============
const char* ssid = "Sarveswar";        // Change this
const char* password = "12345678"; // Change this

// ============ MQTT Configuration ============
const char* mqtt_server = "broker.hivemq.com";  // Free public MQTT broker
const int mqtt_port = 1883;
const char* mqtt_user = "";                  // Leave empty for public broker
const char* mqtt_password = "";              // Leave empty for public broker

// MQTT Topics
const char* LOCK_STATUS_TOPIC = "doorlock/status";
const char* LOCK_COMMAND_TOPIC = "doorlock/command";
const char* LOCK_PASSWORD_TOPIC = "doorlock/password";
const char* LOCK_ALERT_TOPIC = "doorlock/alert";

// ============ Password Configuration ============
String SYSTEM_PASSWORD = "1234";  // Default password
String inputPassword = "";
int failedAttempts = 0;
const int MAX_ATTEMPTS = 3;
bool systemLocked = false;
unsigned long lockoutTime = 0;
const unsigned long LOCKOUT_DURATION = 30000;

// ============ Servo Configuration ============
Servo doorLockServo;
const int SERVO_PIN = 13;
const int SERVO_LOCK_POSITION = 0;
const int SERVO_UNLOCK_POSITION = 90;
bool doorStatus = false; // false = locked, true = unlocked

// ============ Keypad Configuration ============
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

byte rowPins[ROWS] = {19, 18, 15, 14};
byte colPins[COLS] = {27, 26, 25, 33};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ============ Pin Definitions ============
const int GREEN_LED = 4;
const int RED_LED = 5;
const int BUZZER_PIN = 22;

// ============ MQTT and WiFi Objects ============
WiFiClient espClient;
PubSubClient client(espClient);

// ============ System States ============
enum SystemState {
  SYS_IDLE,
  SYS_ENTER_PASSWORD,
  SYS_ACCESS_GRANTED,
  SYS_ACCESS_DENIED
};

SystemState currentState = SYS_IDLE;
unsigned long stateTimer = 0;

// ============ Buzzer Tones ============
const int SUCCESS_TONE1 = 1000;
const int SUCCESS_TONE2 = 1500;
const int ERROR_TONE = 500;
const int ALARM_TONE = 800;
const int LOCK_TONE = 800;

// ============ Setup Function ============
void setup() {
  Serial.begin(115200);
  
  // Initialize pins
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, LOW);
  
  // Initialize Servo
  doorLockServo.attach(SERVO_PIN);
  doorLockServo.write(SERVO_LOCK_POSITION);
  
  // Connect to WiFi
  setupWiFi();
  
  // Setup MQTT
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
  connectMQTT();
  
  // Welcome message
  Serial.println("\n=== ESP32 DOOR LOCK SYSTEM with MQTT ===");
  Serial.println("Door LOCKED");
  Serial.print("Enter Password: ");
  
  currentState = SYS_ENTER_PASSWORD;
  
  // Send initial status
  sendStatusUpdate();
}

// ============ WiFi Connection ============
void setupWiFi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println();
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

// ============ MQTT Connection ============
void connectMQTT() {
  while (!client.connected()) {
    Serial.print("Connecting to MQTT...");
    String clientId = "ESP32DoorLock-" + String(random(0xffff), HEX);
    
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_password)) {
      Serial.println("connected!");
      
      // Subscribe to topics
      client.subscribe(LOCK_COMMAND_TOPIC);
      client.subscribe(LOCK_PASSWORD_TOPIC);
      Serial.println("Subscribed to MQTT topics");
      
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

// ============ MQTT Callback ============
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  Serial.print("MQTT Message received on topic: ");
  Serial.print(topic);
  Serial.print(" - Message: ");
  Serial.println(message);
  
  if (String(topic) == LOCK_COMMAND_TOPIC) {
    if (message == "UNLOCK") {
      if (!doorStatus && !systemLocked) {
        remoteUnlock();
      }
    } else if (message == "LOCK") {
      if (doorStatus) {
        remoteLock();
      }
    } else if (message == "RESET") {
      remoteReset();
    }
  }
  else if (String(topic) == LOCK_PASSWORD_TOPIC) {
    if (message.length() >= 4 && message.length() <= 6) {
      changePasswordRemote(message);
    }
  }
}

// ============ Remote Functions ============
void remoteUnlock() {
  Serial.println("Remote unlock command received");
  grantAccess();
  sendAlert("Door unlocked remotely");
}

void remoteLock() {
  Serial.println("Remote lock command received");
  lockDoor();
  sendAlert("Door locked remotely");
}

void remoteReset() {
  Serial.println("Remote reset command received");
  emergencyReset();
  sendAlert("System reset remotely");
}

void changePasswordRemote(String newPassword) {
  SYSTEM_PASSWORD = newPassword;
  Serial.print("Password changed remotely to: ");
  Serial.println(newPassword);
  sendAlert("Password changed remotely");
  
  // Send confirmation
  DynamicJsonDocument doc(200);
  doc["type"] = "password_changed";
  doc["status"] = "success";
  String output;
  serializeJson(doc, output);
  client.publish(LOCK_ALERT_TOPIC, output.c_str());
}

// ============ Send Status Update ============
void sendStatusUpdate() {
  DynamicJsonDocument doc(256);
  doc["door_status"] = doorStatus ? "UNLOCKED" : "LOCKED";
  doc["system_locked"] = systemLocked;
  doc["failed_attempts"] = failedAttempts;
  doc["state"] = currentState;
  
  String output;
  serializeJson(doc, output);
  client.publish(LOCK_STATUS_TOPIC, output.c_str());
  
  Serial.println("Status update sent to MQTT");
}

void sendAlert(String alert) {
  DynamicJsonDocument doc(200);
  doc["type"] = "alert";
  doc["message"] = alert;
  doc["timestamp"] = millis();
  
  String output;
  serializeJson(doc, output);
  client.publish(LOCK_ALERT_TOPIC, output.c_str());
  
  Serial.print("Alert sent: ");
  Serial.println(alert);
}

// ============ Main Loop ============
void loop() {
  // Maintain MQTT connection
  if (!client.connected()) {
    connectMQTT();
  }
  client.loop();
  
  // Check lockout
  if (systemLocked) {
    if (millis() - lockoutTime >= LOCKOUT_DURATION) {
      systemLocked = false;
      failedAttempts = 0;
      Serial.println("\n=== SYSTEM UNLOCKED ===");
      Serial.print("\nEnter Password: ");
      inputPassword = "";
      sendStatusUpdate();
    }
    return;
  }
  
  // Handle state timeouts
  if (currentState == SYS_ACCESS_GRANTED && millis() - stateTimer >= 5000) {
    lockDoor();
    currentState = SYS_IDLE;
    Serial.println("\n\n=== DOOR LOCKED ===");
    Serial.print("\nEnter Password: ");
    currentState = SYS_ENTER_PASSWORD;
    inputPassword = "";
  }
  
  if (currentState == SYS_ACCESS_DENIED && millis() - stateTimer >= 2000) {
    currentState = SYS_ENTER_PASSWORD;
    Serial.print("\nEnter Password: ");
    inputPassword = "";
  }
  
  if (currentState == SYS_ENTER_PASSWORD) {
    char key = keypad.getKey();
    
    if (key) {
      if (key == '#') {
        Serial.println();
        if (inputPassword == SYSTEM_PASSWORD) {
          grantAccess();
        } else {
          denyAccess();
        }
        inputPassword = "";
      } 
      else if (key == '*') {
        inputPassword = "";
        Serial.print("\rEnter Password:                ");
        Serial.print("\rEnter Password: ");
        tone(BUZZER_PIN, 800, 100);
      }
      else if (key >= '0' && key <= '9') {
        if (inputPassword.length() < 6) {
          inputPassword += key;
          Serial.print("*");
        }
      }
      else if (key == 'A') {
        showSystemInfo();
      }
      else if (key == 'B') {
        changePasswordLocal();
      }
      else if (key == 'C') {
        testServo();
      }
      else if (key == 'D') {
        emergencyReset();
      }
    }
  }
}

void grantAccess() {
  currentState = SYS_ACCESS_GRANTED;
  stateTimer = millis();
  failedAttempts = 0;
  
  doorLockServo.write(SERVO_UNLOCK_POSITION);
  doorStatus = true;
  digitalWrite(GREEN_LED, HIGH);
  
  tone(BUZZER_PIN, SUCCESS_TONE1, 200);
  delay(200);
  tone(BUZZER_PIN, SUCCESS_TONE2, 200);
  delay(100);
  noTone(BUZZER_PIN);
  
  Serial.println("\n✓✓✓ ACCESS GRANTED! ✓✓✓");
  Serial.println("Door UNLOCKED - You have 5 seconds");
  
  sendStatusUpdate();
  sendAlert("Door unlocked - Access granted");
}

void denyAccess() {
  currentState = SYS_ACCESS_DENIED;
  stateTimer = millis();
  failedAttempts++;
  
  digitalWrite(RED_LED, HIGH);
  
  for(int i = 0; i < 2; i++) {
    tone(BUZZER_PIN, ERROR_TONE, 300);
    delay(400);
  }
  noTone(BUZZER_PIN);
  
  Serial.println("\n✗✗✗ ACCESS DENIED! ✗✗✗");
  Serial.print("Attempt: ");
  Serial.print(failedAttempts);
  Serial.print("/");
  Serial.println(MAX_ATTEMPTS);
  
  if (failedAttempts >= MAX_ATTEMPTS) {
    systemLocked = true;
    lockoutTime = millis();
    Serial.println("\n⚠⚠⚠ SYSTEM LOCKED! ⚠⚠⚠");
    
    for(int i = 0; i < 5; i++) {
      tone(BUZZER_PIN, ALARM_TONE, 200);
      delay(300);
    }
    noTone(BUZZER_PIN);
    
    sendAlert("System locked due to multiple failed attempts");
  }
  
  sendStatusUpdate();
  delay(2000);
  digitalWrite(RED_LED, LOW);
}

void lockDoor() {
  doorLockServo.write(SERVO_LOCK_POSITION);
  doorStatus = false;
  digitalWrite(GREEN_LED, LOW);
  
  tone(BUZZER_PIN, LOCK_TONE, 100);
  delay(100);
  noTone(BUZZER_PIN);
  
  Serial.println("Door Locked");
  sendStatusUpdate();
}

void changePasswordLocal() {
  Serial.println("\n=== CHANGE PASSWORD ===");
  Serial.print("Enter current password: ");
  
  String verifyPassword = "";
  char key;
  bool valid = false;
  
  while (!valid) {
    key = keypad.getKey();
    if (key) {
      if (key == '#') {
        if (verifyPassword == SYSTEM_PASSWORD) {
          valid = true;
        } else {
          Serial.println("\nInvalid current password!");
          Serial.print("\nEnter Password: ");
          tone(BUZZER_PIN, ERROR_TONE, 500);
          return;
        }
      }
      else if (key == '*') {
        verifyPassword = "";
        Serial.print("\rEnter current password: ");
      }
      else if (key >= '0' && key <= '9' && verifyPassword.length() < 6) {
        verifyPassword += key;
        Serial.print("*");
      }
    }
  }
  
  Serial.println("\nEnter new password (4-6 digits): ");
  String newPassword = "";
  while (newPassword.length() < 4) {
    key = keypad.getKey();
    if (key && key >= '0' && key <= '9') {
      newPassword += key;
      Serial.print("*");
    }
    else if (key == '*') {
      newPassword = "";
      Serial.print("\rEnter new password: ");
    }
  }
  
  Serial.println("\nConfirm new password: ");
  String confirmPassword = "";
  while (confirmPassword.length() < newPassword.length()) {
    key = keypad.getKey();
    if (key && key >= '0' && key <= '9') {
      confirmPassword += key;
      Serial.print("*");
    }
    else if (key == '*') {
      confirmPassword = "";
      Serial.print("\rConfirm new password: ");
    }
  }
  
  if (newPassword == confirmPassword) {
    SYSTEM_PASSWORD = newPassword;
    Serial.println("\n✓ Password changed successfully!");
    tone(BUZZER_PIN, SUCCESS_TONE1, 150);
    delay(150);
    tone(BUZZER_PIN, SUCCESS_TONE2, 150);
    sendAlert("Password changed locally");
  } else {
    Serial.println("\n✗ Passwords don't match!");
    tone(BUZZER_PIN, ERROR_TONE, 500);
  }
  
  Serial.print("\nEnter Password: ");
}

void testServo() {
  Serial.println("\n=== TESTING SERVO MOTOR ===");
  doorLockServo.write(0);
  delay(1000);
  doorLockServo.write(45);
  delay(1000);
  doorLockServo.write(90);
  delay(1000);
  doorLockServo.write(0);
  Serial.println("Servo test complete\n");
  tone(BUZZER_PIN, 1000, 200);
  Serial.print("Enter Password: ");
}

void emergencyReset() {
  Serial.println("\n⚠ EMERGENCY RESET ⚠");
  doorLockServo.write(SERVO_LOCK_POSITION);
  doorStatus = false;
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, HIGH);
  
  inputPassword = "";
  failedAttempts = 0;
  systemLocked = false;
  currentState = SYS_ENTER_PASSWORD;
  
  for(int i = 0; i < 3; i++) {
    tone(BUZZER_PIN, 1000, 200);
    delay(300);
  }
  noTone(BUZZER_PIN);
  
  Serial.println("\n✓ System Reset Complete!");
  Serial.print("\nEnter Password: ");
  
  delay(2000);
  digitalWrite(RED_LED, LOW);
  sendStatusUpdate();
  sendAlert("Emergency reset performed");
}

void showSystemInfo() {
  Serial.println("\n=== SYSTEM INFORMATION ===");
  Serial.print("WiFi Status: ");
  Serial.println(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  Serial.print("MQTT Server: ");
  Serial.println(mqtt_server);
  Serial.print("Door Status: ");
  Serial.println(doorStatus ? "UNLOCKED" : "LOCKED");
  Serial.print("System Locked: ");
  Serial.println(systemLocked ? "YES" : "NO");
  Serial.print("Failed Attempts: ");
  Serial.println(failedAttempts);
  Serial.println("========================\n");
  Serial.print("Enter Password: ");
}