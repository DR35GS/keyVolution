#include <WiFi.h>
#include <esp_now.h>
#include <SPI.h>
#include <MFRC522.h>
#include "esp_sleep.h"
#include <esp_wifi.h>

// ---------- PINS ----------
#define SS_PIN   5
#define RST_PIN  27

#define LED_G 16
#define LED_R 17
#define BUZZ  25
#define BTN   33   // Wake & Doorbell button
#define IRQ_PIN 32 // FUTURE: Connect MFRC522 IRQ pin here (Must be an RTC GPIO)

// ---------- TIMERS & CACHE ----------
#define AWAKE_TIME_MS  10000 // Stay awake for 10 seconds after button press
#define RX_TIMEOUT_MS  2000 

// RTC Memory survives Deep Sleep to keep connection fast
RTC_DATA_ATTR int32_t savedChannel = 0;

// ---------- BOX 2 MAC ----------
uint8_t box2Mac[] = {0x08,0xB6,0x1F,0xB6,0x1C,0xBC}; // Make sure this matches Box 2!

// ---------- MESSAGE TYPES ----------
enum MsgType : uint8_t {
  MSG_NONE     = 0,
  MSG_DOORBELL = 1,
  MSG_RFID     = 2,
  MSG_RFID_OK  = 10,
  MSG_RFID_BLOCKED = 11,
  MSG_RFID_DENIED  = 12
};

struct __attribute__((packed)) EspNowMsg {
  uint8_t type;
  uint8_t uid[10];
};

// ---------- GLOBALS ----------
MFRC522 rfid(SS_PIN, RST_PIN);
volatile bool responseReceived = false;
volatile MsgType incomingResp = MSG_NONE;
bool buttonReleasedAfterWake = false;
unsigned long awakeTimer = 0;

// ---------- HELPERS ----------
void beep(uint16_t ms) { digitalWrite(BUZZ,HIGH); delay(ms); digitalWrite(BUZZ,LOW); }

// Updated to new ESP-NOW Core v3 signature
void onBox2Response(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if(len<1) return;
  incomingResp = (MsgType)data[0];
  responseReceived = true;
}

void sendMsg(EspNowMsg &msg) { esp_now_send(box2Mac, (uint8_t*)&msg, sizeof(msg)); }

void goToSleep() {
  Serial.println("Going to Deep Sleep...");
  digitalWrite(LED_G, LOW); 
  digitalWrite(LED_R, LOW);
  delay(100); // Allow serial to flush
  
  // --- CURRENT WAKE METHOD (BUTTON) ---
  // Wakes ESP32 when BTN (GPIO 33) is pulled LOW
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN, 0); 

  // --- FUTURE WAKE METHOD (RFID IRQ) ---
  // To use this later, wire the MFRC522 IRQ pin to GPIO 32 and uncomment below:
  /*
  rfid.PCD_WriteRegister(rfid.ComIEnReg, 0xA0); // Enable Rx IRQ
  rfid.PCD_WriteRegister(rfid.DivIEnReg, 0x80); // Enable IRQ pin
  esp_sleep_enable_ext1_wakeup(1ULL << IRQ_PIN, ESP_EXT1_WAKEUP_ALL_LOW); 
  */

  esp_deep_sleep_start();
}

void handleFeedback(MsgType resp){
  switch(resp){
    case MSG_RFID_OK:
      Serial.println("Access Granted");
      digitalWrite(LED_R,LOW); digitalWrite(LED_G,HIGH);
      tone(BUZZ,2000,150); delay(200); tone(BUZZ,3000,150); delay(1000); break;
    case MSG_RFID_BLOCKED:
      Serial.println("Access Blocked");
      for(int i=0;i<3;i++){ digitalWrite(LED_R,HIGH); tone(BUZZ,200,300); delay(300); digitalWrite(LED_R,LOW); delay(100);}
      break;
    case MSG_RFID_DENIED:
      Serial.println("Access Denied");
      for(int i=0;i<2;i++){ digitalWrite(LED_R,HIGH); tone(BUZZ,500,150); delay(200); digitalWrite(LED_R,LOW); delay(100);}
      break;
    default: break;
  }
  digitalWrite(LED_R,LOW); digitalWrite(LED_G,LOW);
}

// ---------- CHANNEL SYNC ----------
int32_t getBox2Channel() {
  Serial.println("Scanning for Box 2...");
  int n = WiFi.scanNetworks(false, true); 
  for (int i = 0; i < n; i++) {
    // FIXED: Now correctly matches Box 2's AP name
    if (WiFi.SSID(i) == "KeyVolution_Setup") return WiFi.channel(i);
  }
  return 0; 
}

// ---------- SETUP ----------
void setup(){
  Serial.begin(115200);

  pinMode(LED_R,OUTPUT); pinMode(LED_G,OUTPUT); pinMode(BUZZ,OUTPUT);
  pinMode(BTN,INPUT_PULLUP);
  // pinMode(IRQ_PIN, INPUT_PULLUP); // FUTURE: Uncomment for IRQ

  // ESP-NOW Sync
  WiFi.mode(WIFI_STA);
  if (savedChannel == 0) savedChannel = getBox2Channel();

  if (savedChannel > 0) {
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(savedChannel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
  }

  if(esp_now_init()!=ESP_OK) { goToSleep(); return; }
  esp_now_register_recv_cb(onBox2Response);

  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, box2Mac,6); 
  peer.channel = (savedChannel > 0) ? savedChannel : 0; 
  peer.encrypt=false;
  esp_now_add_peer(&peer);

  // RFID Init
  SPI.begin(); rfid.PCD_Init();

  // Indicate Wake State to User (Quick Flash)
  digitalWrite(LED_R, HIGH); digitalWrite(LED_G, HIGH); delay(50);
  digitalWrite(LED_R, LOW); digitalWrite(LED_G, LOW);
  
  // Clear any pending RFID interrupts from previous wakes
  // rfid.PCD_WriteRegister(rfid.ComIrqReg, 0x7F); // FUTURE: Uncomment for IRQ

  awakeTimer = millis();
}

// ---------- LOOP ----------
void loop(){
  // 1. Check if our 10-second window is over
  if (millis() - awakeTimer > AWAKE_TIME_MS) {
    goToSleep();
  }

  // 2. Doorbell Logic
  // Wait until the user lets go of the button from the initial "wake" press
  if (digitalRead(BTN) == HIGH) {
    buttonReleasedAfterWake = true; 
  }
  
  // If they press it AGAIN while awake, ring the doorbell
  if (buttonReleasedAfterWake && digitalRead(BTN) == LOW) {
    Serial.println("Doorbell Triggered!");
    EspNowMsg msg{};
    msg.type = MSG_DOORBELL;
    sendMsg(msg);
    digitalWrite(LED_R,HIGH); beep(100); digitalWrite(LED_R,LOW);
    
    buttonReleasedAfterWake = false; // Require release again
    awakeTimer = millis(); // Extend the sleep timer since they are interacting
    delay(500); // Debounce
  }

  // 3. RFID Logic
  if(rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    EspNowMsg msg{};
    msg.type = MSG_RFID;
    memset(msg.uid, 0, sizeof(msg.uid));
    memcpy(msg.uid, rfid.uid.uidByte, rfid.uid.size);

    digitalWrite(LED_G,HIGH); beep(50); digitalWrite(LED_G,LOW);
    
    responseReceived = false;
    incomingResp = MSG_NONE;
    sendMsg(msg);

    // Wait for Box 2 Reply
    unsigned long start = millis();
    bool gotReply = false;
    while(millis() - start < RX_TIMEOUT_MS){
      if(responseReceived){
        handleFeedback(incomingResp);
        gotReply = true;
        break;
      }
      delay(10);
    }
    
    // Failsafe for dropped connections
    if(!gotReply){
      Serial.println("Timeout waiting for Box2. Resetting channel cache.");
      tone(BUZZ,200,500);
      savedChannel = 0; // Force a Wi-Fi scan on the next wake up
    }
    
    goToSleep(); // Immediately sleep after processing the card
  }
}