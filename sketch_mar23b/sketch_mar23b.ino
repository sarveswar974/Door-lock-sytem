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
const unsigned long LOCKOUT_DURATION = 30000; // 30 seconds

// ============ Servo Configuration ============
Servo doorLockServo;
const int SERVO_PIN = 13;
const int SERVO_LOCK_POSITION = 0;
const int SERVO_UNLOCK_POSITION = 90;
bool doorStatus = false; // false = locked, true = unlocked

// ============ Unlock Timing Configuration ============
const unsigned long UNLOCK_DURATION = 5000; // Door stays unlocked for 5 seconds
unsigned long unlockStartTime = 0;
bool unlockTimerActive = false;

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
  SYS_ACCESS_DENIED,
  SYS_CHANGE_PASSWORD_CURRENT,
  SYS_CHANGE_PASSWORD_NEW,
  SYS_CHANGE_PASSWORD_CONFIRM
};

SystemState currentState = SYS_IDLE;
unsigned long stateTimer = 0;

// ============ Password Change Variables ============
String currentPasswordVerify = "";
String newPassword = "";
String confirmPassword = "";
bool passwordChangeMode = false;

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

// ============ Display Lockout Countdown ============
void displayLockoutCountdown() {
  unsigned long elapsed = millis() - lockoutTime;
  unsigned long remaining = LOCKOUT_DURATION - elapsed;
  unsigned long secondsRemaining = (remaining + 999) / 1000; // Round up
  
  static unsigned long lastDisplayTime = 0;
  static int lastSecondsRemaining = -1;
  
  // Update display every 100ms for smooth countdown
  if (millis() - lastDisplayTime >= 100) {
    lastDisplayTime = millis();
    
    // Only update if seconds changed or every 100ms for smooth display
    if (secondsRemaining != lastSecondsRemaining) {
      lastSecondsRemaining = secondsRemaining;
      
      // Clear the line and show countdown
      Serial.print("\r");
      Serial.print("System locked! Time remaining: ");
      Serial.print(secondsRemaining);
      Serial.print(" seconds   ");
      
      // Play beep for each second
      if (elapsed % 1000 < 100 && secondsRemaining > 0) {
        tone(BUZZER_PIN, ALARM_TONE, 50);
      }
    }
    
    // Blink red LED during lockout
    digitalWrite(RED_LED, !digitalRead(RED_LED));
  }
}

// ============ Main Loop ============
void loop() {
  // Maintain MQTT connection
  if (!client.connected()) {
    connectMQTT();
  }
  client.loop();
  
  // Handle unlock timer
  if (unlockTimerActive && (millis() - unlockStartTime >= UNLOCK_DURATION)) {
    lockDoor();
    unlockTimerActive = false;
    currentState = SYS_IDLE;
    Serial.println("\n\n=== DOOR LOCKED ===");
    Serial.print("\nEnter Password: ");
    currentState = SYS_ENTER_PASSWORD;
    inputPassword = "";
  }
  
  // Check lockout with countdown display
  if (systemLocked) {
    displayLockoutCountdown();
    
    if (millis() - lockoutTime >= LOCKOUT_DURATION) {
      systemLocked = false;
      failedAttempts = 0;
      digitalWrite(RED_LED, LOW);
      Serial.println("\n\n=== SYSTEM UNLOCKED ===");
      Serial.println("You can now enter password again");
      Serial.print("\nEnter Password: ");
      inputPassword = "";
      sendStatusUpdate();
      
      // Play success tone
      tone(BUZZER_PIN, SUCCESS_TONE1, 200);
      delay(200);
      tone(BUZZER_PIN, SUCCESS_TONE2, 200);
      delay(100);
      noTone(BUZZER_PIN);
    }
    return;
  }
  
  // Handle state timeouts
  if (currentState == SYS_ACCESS_DENIED && millis() - stateTimer >= 2000) {
    currentState = SYS_ENTER_PASSWORD;
    Serial.print("\nEnter Password: ");
    inputPassword = "";
    digitalWrite(RED_LED, LOW);
  }
  
  // Handle password change timeouts
  if (passwordChangeMode && millis() - stateTimer >= 30000) {
    // Timeout after 30 seconds of inactivity
    passwordChangeMode = false;
    currentState = SYS_ENTER_PASSWORD;
    Serial.println("\n\nPassword change cancelled (timeout)");
    Serial.print("\nEnter Password: ");
    inputPassword = "";
  }
  
  // Handle keypad input based on current state
  char key = keypad.getKey();
  
  if (key) {
    // Reset timeout timer for password change mode
    if (passwordChangeMode) {
      stateTimer = millis();
    }
    
    switch (currentState) {
      case SYS_ENTER_PASSWORD:
        handlePasswordEntry(key);
        break;
        
      case SYS_CHANGE_PASSWORD_CURRENT:
        handleCurrentPasswordEntry(key);
        break;
        
      case SYS_CHANGE_PASSWORD_NEW:
        handleNewPasswordEntry(key);
        break;
        
      case SYS_CHANGE_PASSWORD_CONFIRM:
        handleConfirmPasswordEntry(key);
        break;
    }
    
    // Handle function keys (only in normal mode)
    if (currentState == SYS_ENTER_PASSWORD) {
      if (key == 'A') {
        showSystemInfo();
      }
      else if (key == 'B') {
        startPasswordChange();
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

void handlePasswordEntry(char key) {
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
}

void startPasswordChange() {
  if (systemLocked) {
    Serial.println("\nCannot change password - System locked!");
    tone(BUZZER_PIN, ERROR_TONE, 500);
    return;
  }
  
  passwordChangeMode = true;
  currentState = SYS_CHANGE_PASSWORD_CURRENT;
  stateTimer = millis();
  currentPasswordVerify = "";
  
  Serial.println("\n\n=== CHANGE PASSWORD ===");
  Serial.print("Enter CURRENT password: ");
}

void handleCurrentPasswordEntry(char key) {
  if (key == '#') {
    if (currentPasswordVerify == SYSTEM_PASSWORD) {
      // Current password correct, proceed to new password
      currentState = SYS_CHANGE_PASSWORD_NEW;
      newPassword = "";
      Serial.println("\n✓ Current password verified");
      Serial.print("Enter NEW password (4-6 digits): ");
      tone(BUZZER_PIN, SUCCESS_TONE1, 100);
    } else {
      // Incorrect current password
      Serial.println("\n✗ Incorrect current password!");
      tone(BUZZER_PIN, ERROR_TONE, 500);
      passwordChangeMode = false;
      currentState = SYS_ENTER_PASSWORD;
      Serial.print("\nEnter Password: ");
      inputPassword = "";
    }
    currentPasswordVerify = "";
  }
  else if (key == '*') {
    // Cancel password change
    currentPasswordVerify = "";
    passwordChangeMode = false;
    currentState = SYS_ENTER_PASSWORD;
    Serial.println("\nPassword change cancelled");
    Serial.print("\nEnter Password: ");
    inputPassword = "";
    tone(BUZZER_PIN, 800, 100);
  }
  else if (key >= '0' && key <= '9') {
    if (currentPasswordVerify.length() < 6) {
      currentPasswordVerify += key;
      Serial.print("*");
    }
  }
}

void handleNewPasswordEntry(char key) {
  if (key == '#') {
    if (newPassword.length() >= 4 && newPassword.length() <= 6) {
      // Valid password length, proceed to confirm
      currentState = SYS_CHANGE_PASSWORD_CONFIRM;
      confirmPassword = "";
      Serial.println("\n✓ Password accepted");
      Serial.print("CONFIRM new password: ");
    } else {
      // Invalid password length
      Serial.println("\n✗ Password must be 4-6 digits!");
      tone(BUZZER_PIN, ERROR_TONE, 500);
      newPassword = "";
      Serial.print("Enter NEW password (4-6 digits): ");
    }
  }
  else if (key == '*') {
    // Cancel password change
    newPassword = "";
    passwordChangeMode = false;
    currentState = SYS_ENTER_PASSWORD;
    Serial.println("\nPassword change cancelled");
    Serial.print("\nEnter Password: ");
    inputPassword = "";
    tone(BUZZER_PIN, 800, 100);
  }
  else if (key >= '0' && key <= '9') {
    if (newPassword.length() < 6) {
      newPassword += key;
      Serial.print("*");
    }
  }
}

void handleConfirmPasswordEntry(char key) {
  if (key == '#') {
    if (confirmPassword == newPassword) {
      // Passwords match, update the system password
      SYSTEM_PASSWORD = newPassword;
      Serial.println("\n✓✓✓ PASSWORD CHANGED SUCCESSFULLY! ✓✓✓");
      tone(BUZZER_PIN, SUCCESS_TONE1, 150);
      delay(150);
      tone(BUZZER_PIN, SUCCESS_TONE2, 150);
      delay(100);
      noTone(BUZZER_PIN);
      
      sendAlert("Password changed locally");
      
      // Reset and return to normal mode
      passwordChangeMode = false;
      currentState = SYS_ENTER_PASSWORD;
      Serial.print("\nEnter Password: ");
      inputPassword = "";
    } else {
      // Passwords don't match
      Serial.println("\n✗✗✗ PASSWORDS DO NOT MATCH! ✗✗✗");
      tone(BUZZER_PIN, ERROR_TONE, 500);
      delay(500);
      tone(BUZZER_PIN, ERROR_TONE, 500);
      
      // Restart password change process
      currentState = SYS_CHANGE_PASSWORD_NEW;
      newPassword = "";
      Serial.print("\nEnter NEW password (4-6 digits): ");
    }
    confirmPassword = "";
  }
  else if (key == '*') {
    // Cancel password change
    confirmPassword = "";
    passwordChangeMode = false;
    currentState = SYS_ENTER_PASSWORD;
    Serial.println("\nPassword change cancelled");
    Serial.print("\nEnter Password: ");
    inputPassword = "";
    tone(BUZZER_PIN, 800, 100);
  }
  else if (key >= '0' && key <= '9') {
    if (confirmPassword.length() < 6) {
      confirmPassword += key;
      Serial.print("*");
    }
  }
}

void grantAccess() {
  currentState = SYS_ACCESS_GRANTED;
  unlockStartTime = millis();
  unlockTimerActive = true;
  failedAttempts = 0;
  
  doorLockServo.write(SERVO_UNLOCK_POSITION);
  doorStatus = true;
  digitalWrite(GREEN_LED, HIGH);
  digitalWrite(RED_LED, LOW);
  
  tone(BUZZER_PIN, SUCCESS_TONE1, 200);
  delay(200);
  tone(BUZZER_PIN, SUCCESS_TONE2, 200);
  delay(100);
  noTone(BUZZER_PIN);
  
  Serial.println("\n✓✓✓ ACCESS GRANTED! ✓✓✓");
  Serial.print("Door UNLOCKED - Will lock automatically in ");
  Serial.print(UNLOCK_DURATION / 1000);
  Serial.println(" seconds");
  
  sendStatusUpdate();
  sendAlert("Door unlocked - Access granted");
}

void denyAccess() {
  currentState = SYS_ACCESS_DENIED;
  stateTimer = millis();
  failedAttempts++;
  
  digitalWrite(RED_LED, HIGH);
  digitalWrite(GREEN_LED, LOW);
  
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
    Serial.print("System will be locked for ");
    Serial.print(LOCKOUT_DURATION / 1000);
    Serial.println(" seconds");
    
    for(int i = 0; i < 5; i++) {
      tone(BUZZER_PIN, ALARM_TONE, 200);
      delay(300);
    }
    noTone(BUZZER_PIN);
    
    sendAlert("System locked due to multiple failed attempts");
  }
  
  sendStatusUpdate();
}

void lockDoor() {
  if (doorStatus) {  // Only lock if currently unlocked
    doorLockServo.write(SERVO_LOCK_POSITION);
    doorStatus = false;
    digitalWrite(GREEN_LED, LOW);
    
    tone(BUZZER_PIN, LOCK_TONE, 100);
    delay(100);
    noTone(BUZZER_PIN);
    
    Serial.println("\nDoor Locked");
    sendStatusUpdate();
  }
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
  unlockTimerActive = false;
  passwordChangeMode = false;
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
  Serial.print("Unlock Duration: ");
  Serial.print(UNLOCK_DURATION / 1000);
  Serial.println(" seconds");
  Serial.print("Lockout Duration: ");
  Serial.print(LOCKOUT_DURATION / 1000);
  Serial.println(" seconds");
  Serial.print("Current Password: ");
  Serial.println(SYSTEM_PASSWORD); // Only show in serial monitor for debugging
  Serial.println("========================\n");
  Serial.print("Enter Password: ");
}