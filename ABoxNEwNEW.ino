#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <time.h>
#include <map>
#include <vector>
#include <Update.h>

// --- BOX 1 COMPATIBILITY LAYER ---
struct __attribute__((packed)) EspNowMsg {
  uint8_t type;    
  uint8_t uid[10]; 
};
#define MSG_RFID_OK      10
#define MSG_RFID_BLOCKED 11
#define MSG_RFID_DENIED  12
#define MSG_PING         98 
#define MSG_SYNC_CHANNEL 99 
uint8_t box1Mac[] = {0x08, 0xA6, 0xF7, 0x66, 0x38, 0xD4};

// ---------- PINS ----------
#define RESET_BTN 2
#define SOLENOID 26
#define BUZZ     25
#define TOUCH    32
#define OVERRIDE 33  
#define LED_RED  16
#define LED_GREEN 17

// ---------- GLOBALS ----------
WiFiClientSecure secured_client;
UniversalTelegramBot* bot = nullptr; 
WebServer server(80);

String devicePass = "Admin123";
String botToken = ""; 
String currentSSID = "";     
String currentWIFIPASS = ""; 
unsigned long autoLockMS = 30000;
String currentMelody = "880,150;1047,150;1318,150";
bool isMuted = false;
int timezoneOffset = 8; 
int maxLogLines = 150; 

// Pirates of the Caribbean (Continuous Version)
enum DoorState { LOCKED, UNLOCKED };
DoorState doorState = LOCKED;
unsigned long unlockTime = 0;
bool botAuth = false;

// Hardware status
unsigned long lastTouchTime = 0;
unsigned long lastBotCheck = 0;
bool scannerAwake = false; 

// Touch Reset Logic
bool isTouching = false;
unsigned long touchStartTime = 0;
unsigned long lastWarningBeep = 0;

// WiFi Mgmt
unsigned long lastWiFiCheck = 0;
bool apActive = true;
uint8_t lastChannel = 0;

// Audio State Machine
bool melodyPlaying = false;
String melodyQueue = "";
unsigned long nextNoteTime = 0;

// RAM Key Cache
struct KeyData { String name; String type; };
std::map<String, KeyData> keyCache;

// ---------- STORAGE & LOGGING ----------
void saveConfig() {
  File f = LittleFS.open("/config.json", "w");
  StaticJsonDocument<1024> doc; 
  doc["pass"] = devicePass;
  doc["botToken"] = botToken;
  doc["ssid"] = currentSSID;
  doc["wifiPass"] = currentWIFIPASS;
  doc["timer"] = autoLockMS; 
  doc["melody"] = currentMelody;
  doc["muted"] = isMuted;
  doc["tz"] = timezoneOffset;
  doc["logLimit"] = maxLogLines;
  serializeJson(doc, f); 
  f.close();
}

void loadConfig() {
  if (!LittleFS.exists("/config.json")) return;
  File f = LittleFS.open("/config.json", "r");
  StaticJsonDocument<1024> doc;
  deserializeJson(doc, f);
  devicePass = doc["pass"] | "Admin123";
  botToken = doc["botToken"] | "";
  currentSSID = doc["ssid"] | "";
  currentWIFIPASS = doc["wifiPass"] | "";
  autoLockMS = doc["timer"] | 30000; 
  currentMelody = doc["melody"] | "880,150;1047,150;1318,150";
  isMuted = doc["muted"] | false;
  timezoneOffset = doc["tz"] | 8;
  maxLogLines = doc["logLimit"] | 150;
  f.close();
}

void loadKeysToRAM() {
  keyCache.clear();
  File f = LittleFS.open("/keys.json", "r");
  if (!f) return;
  StaticJsonDocument<2048> doc;
  deserializeJson(doc, f);
  f.close();
  JsonObject root = doc.as<JsonObject>();
  for (JsonPair kv : root) {
    keyCache[String(kv.key().c_str())] = {kv.value()["name"].as<String>(), kv.value()["type"].as<String>()};
  }
}

void logEvent(String msg) {
  struct tm timeinfo;
  String timestamp;
  if (getLocalTime(&timeinfo, 10)) {
    char timeStr[25];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
    timestamp = String(timeStr);
  } else {
    timestamp = String(millis()) + "ms";
  }

  String fullMsg = "[" + timestamp + "] " + msg;
  Serial.println(fullMsg);

  std::vector<String> lines;
  File r = LittleFS.open("/history.txt", "r");
  if (r) {
    while (r.available()) {
      String line = r.readStringUntil('\n');
      line.trim();
      if (line.length() > 0) lines.push_back(line);
    }
    r.close();
  }
  
  lines.push_back(fullMsg);
  while (lines.size() > maxLogLines) lines.erase(lines.begin()); 

  File w = LittleFS.open("/history.txt", "w");
  if (w) {
    for (const String& l : lines) w.println(l);
    w.close();
  }
}

// ---------- ACTIONS ----------
bool overrideActive() { return digitalRead(OVERRIDE) == LOW; }

void startMelody(String m) {
  if (isMuted) return;
  melodyQueue = m;
  melodyPlaying = true;
  nextNoteTime = millis();
}

void handleMelody() {
  if (!melodyPlaying || millis() < nextNoteTime) return;
  if (melodyQueue.length() == 0) { melodyPlaying = false; noTone(BUZZ); return; }

  int semi = melodyQueue.indexOf(';');
  String noteStr = (semi > -1) ? melodyQueue.substring(0, semi) : melodyQueue;
  if (semi > -1) melodyQueue = melodyQueue.substring(semi + 1);
  else melodyQueue = "";

  int comma = noteStr.indexOf(',');
  if (comma > -1) {
    int freq = noteStr.substring(0, comma).toInt();
    int dur = noteStr.substring(comma + 1).toInt();
    if (freq > 0) tone(BUZZ, freq, dur);
    else noTone(BUZZ);
    // Adjusted gap to 50ms to help define continuous notes
    nextNoteTime = millis() + dur + 50; 
  } else { nextNoteTime = millis(); }
}

void handleWiFi() {
  if (currentSSID.length() == 0) return;
  
  if (WiFi.status() == WL_CONNECTED) {
    if (apActive) { WiFi.mode(WIFI_STA); apActive = false; logEvent("WiFi Connected."); }
    
    if (WiFi.channel() != lastChannel && WiFi.channel() > 0) {
      lastChannel = WiFi.channel();
      logEvent("WiFi Channel updated to " + String(lastChannel) + ". Syncing ESP-NOW...");
      EspNowMsg syncMsg; syncMsg.type = MSG_SYNC_CHANNEL; syncMsg.uid[0] = lastChannel;
      esp_now_send(box1Mac, (uint8_t*)&syncMsg, sizeof(syncMsg));
    }
  } else {
    if (millis() - lastWiFiCheck > 10000) {
      WiFi.disconnect(); WiFi.begin(currentSSID.c_str(), currentWIFIPASS.c_str());
      lastWiFiCheck = millis();
    }
  }
}

void lockDoor(String source = "Manual") {
  digitalWrite(SOLENOID, HIGH); digitalWrite(LED_RED, HIGH); digitalWrite(LED_GREEN, LOW);
  doorState = LOCKED; logEvent("LOCKED by " + source);
}

void unlockDoor(String source = "Manual") {
  digitalWrite(SOLENOID, LOW); digitalWrite(LED_RED, LOW); digitalWrite(LED_GREEN, HIGH);
  doorState = UNLOCKED; unlockTime = millis();
  logEvent("UNLOCKED by " + source); startMelody(currentMelody);
}

// ---------- ESP-NOW HANDLERS ----------
void OnDataSent(const esp_now_send_info_t *info, esp_now_send_status_t status) {
  scannerAwake = (status == ESP_NOW_SEND_SUCCESS);
}

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < 1) return;
  EspNowMsg* msg = (EspNowMsg*)data;

  if (msg->type == 1) { 
    logEvent("Doorbell Pressed"); 
    startMelody(currentMelody); //
  } 
  else if (msg->type == 2) {
    char hexUid[21] = {0};
    for(int i=0; i<10; i++) sprintf(hexUid + (i*2), "%02X", msg->uid[i]);
    String uidStr = String(hexUid);
    
    String keyName = "Unregistered"; bool isMaster = false;
    if (keyCache.count(uidStr)) { keyName = keyCache[uidStr].name; isMaster = (keyCache[uidStr].type == "master"); }
    
    uint8_t response;
    if (keyName == "Unregistered") { response = MSG_RFID_DENIED; logEvent("Access Denied: " + uidStr); } 
    else if (overrideActive() && !isMaster) { response = MSG_RFID_BLOCKED; logEvent("Access Blocked: " + keyName); startMelody("400,150"); } 
    else { response = MSG_RFID_OK; unlockDoor(keyName); }
    esp_now_send(box1Mac, &response, 1);
  }
}

// ---------- WEB UI ----------
bool checkAuth() {
  if (!server.authenticate("admin", devicePass.c_str())) { server.requestAuthentication(); return false; }
  return true;
}

const char* index_html = R"rawliteral(
<!DOCTYPE html><html><head><title>KeyVolution Smart Lock</title>
<meta charset="UTF-8">
<meta name='viewport' content='width=device-width, initial-scale=1'>
<style>body{font-family:sans-serif;background:#f4f4f9;padding:20px;}.card{background:white;padding:20px;border-radius:10px;box-shadow:0 2px 5px rgba(0,0,0,0.1);max-width:600px;margin:auto;}
input,select,button{width:100%;padding:10px;margin:5px 0;border-radius:5px;border:1px solid #ddd;box-sizing:border-box;}button{background:#1a73e8;color:white;border:none;cursor:pointer;font-weight:bold;}
#logs,#keys{background:#eee;padding:10px;height:150px;overflow-y:auto;font-size:12px;margin-bottom:10px;border-radius:5px; white-space: pre-wrap;}
table{width:100%;border-collapse:collapse;}th,td{padding:5px;border-bottom:1px solid #ccc;text-align:left;}
.flex{display:flex;gap:5px;align-items:center;} .flex input, .flex select{flex:1;margin:0;} .flex button{width:auto;white-space:nowrap;padding:10px 15px;margin:0;}
h5{margin-bottom:0;} .statbox{background:#e8f0fe;padding:10px;border-radius:5px;display:flex;justify-content:space-between;align-items:center;margin-bottom:15px;}
</style></head><body>
<div class='card'><h2>KeyVolution Smart Lock</h2>
<div class='statbox'><div id='sysStat'>Fetching status...</div><button onclick='s()' style='width:auto;padding:5px 10px;margin:0;background:#aaa;'>↻ Refresh</button></div>

<div class='flex'>
  <button onclick='fetch("/pingScanner",{method:"POST"}).then(r=>r.text()).then(t=>alert(t))' style='background:#34a853'>Ping Scanner (Box 1)</button>
  <button onclick='window.location.href="/ota"' style='background:#f4b400'>OTA Update</button>
</div>

<h4>Keys Management</h4><div id='keys'>Loading...</div>
<div class='flex'><input id='ku' placeholder='UID (Hex)'><input id='kn' placeholder='Person Name'><select id='kt'><option value='master'>Master</option><option value='temp'>Temp</option></select><button onclick='g("/addKey?u="+v("ku")+"&n="+v("kn")+"&t="+v("kt"))'>Add</button></div>
<button onclick='g("/revoke?u="+v("ku"))' style='background:#d93025;margin-top:5px;'>Revoke Key</button>

<h4>Telegram Setup</h4>
<input id='bt' placeholder='Enter new Telegram Bot Token' type='password'>
<button onclick='if(confirm("Are you sure you want to change the Bot Token? The system will reboot to apply changes.")){ fetch("/setToken?t="+encodeURIComponent(v("bt")),{method:"POST"}).then(()=>alert("Rebooting...")); }' style='background:#d93025'>Update Bot Token</button>

<h4>Settings</h4>
<div class='flex'><input id='st' placeholder='Auto-Lock (ms)'><button onclick='g("/setTimer?t="+v("st"))'>Save Timer</button></div>
<div class='flex' style='margin-top:5px;'><input id='ll' placeholder='Max Log Lines (150)'><button onclick='g("/setLogLimit?l="+v("ll"))'>Save Log Limit</button></div>
<div class='flex' style='margin-top:5px;'><select id='smu'><option value='0'>Sound: ON</option><option value='1'>Sound: OFF (Muted)</option></select><button onclick='g("/setSound?mu="+v("smu"))'>Save Sound</button></div>
<div class='flex' style='margin-top:5px;'><input id='tz' placeholder='Timezone Offset (e.g., 8)'><button onclick='g("/setTZ?z="+v("tz"))'>Save Timezone</button></div>

<div class='flex' style='margin-top:5px;'>
  <select id='melSel' onchange='document.getElementById("sm").style.display=(this.value=="custom"?"block":"none")'>
    <option value='880,150;1047,150;1318,150'>1. Default Success</option>
    <option value='1000,100'>2. Short Beep</option>
    <option value='1000,100;0,50;1000,100'>3. Double Beep</option>
    <option value='500,100;1000,100;1500,100'>4. Ascending</option>
    <option value='1500,100;1000,100;500,100'>5. Descending</option>
    <option value='400,150;300,200'>6. Error / Denied</option>
    <option value='988,100;1319,400'>7. Mario Coin</option>
    <option value='1046,100;1318,100;1568,100;2093,200'>8. Happy Fanfare</option>
    <option value='440,200;440,200;440,200'>9. Flat Tone</option>
    <option value='2000,50;0,50;2000,50'>10. Quick Chirp</option>
    <option value='330,187;392,187;440,375;440,187;440,187;494,187;523,375'>11. Pirates Theme</option>
    <option value='custom'>12. Custom Input...</option>
  </select>
  <input id='sm' placeholder='Freq,Dur;Freq,Dur' style='display:none;'>
  <button onclick='let m=(v("melSel")=="custom")?v("sm"):v("melSel"); g("/setMelody?m="+encodeURIComponent(m))'>Save Melody</button>
</div>

<h5>WiFi & Security</h5>
<input id='sw' placeholder='WiFi SSID'><input id='sp' placeholder='WiFi Pass'>
<button onclick='g("/setWiFi?s="+encodeURIComponent(v("sw"))+"&p="+encodeURIComponent(v("sp")))'>Save WiFi</button>
<br>
<input id='dp' placeholder='New Admin Password' type='password'>
<button onclick='let p=v("dp"); if(p){ if(confirm("New Password: "+p+"\n\nPress OK to confirm")){ fetch("/setCreds?p="+encodeURIComponent(p),{method:"POST"}).then(r=>r.text()).then(t=>{alert(t); location.reload();}); } }' style='background:#d93025'>Update Admin Password</button>

<button onclick='if(confirm("Restart the lock?")){fetch("/reboot",{method:"POST"}).then(()=>alert("Rebooting..."));}' style='background:#f4b400;margin-top:10px;'>Reboot System</button>
<button onclick='if(confirm("DANGER: Factory Reset?")){fetch("/factoryReset",{method:"POST"}).then(r=>r.text()).then(t=>{alert(t); location.reload();});}' style='background:#d93025;margin-top:10px;'>Factory Reset</button>

<h4>History</h4><div id='logs'>Loading...</div><button onclick='h()'>Refresh Logs</button></div>
<script>
function v(id){return document.getElementById(id).value;}
function s(){fetch("/statusJSON").then(r=>r.json()).then(d=>{let ds=d.door=="LOCKED"?"🔒 LOCKED":"🔓 UNLOCKED"; let ov=d.override?"🛑 ACTIVE":"✅ OFF"; document.getElementById("sysStat").innerHTML="<b>Door:</b> "+ds+"<br><b>Override:</b> "+ov;});}
function h(){fetch("/history").then(r=>r.text()).then(d=>document.getElementById("logs").innerText=d);}
function fetchKeys(){fetch("/listKeys").then(r=>r.json()).then(d=>{let t="<table><tr><th>Name</th><th>UID</th><th>Type</th></tr>";for(let k in d) t+="<tr><td>"+d[k].name+"</td><td>"+k+"</td><td>"+d[k].type+"</td></tr>";document.getElementById("keys").innerHTML=t+"</table>";}).catch(()=>document.getElementById("keys").innerText="No Keys Found");}
function g(p){fetch(p,{method:"POST"}).then(r=>r.text()).then(t=>{alert(t); h(); s(); fetchKeys();});}
fetchKeys(); h(); s(); setInterval(s, 5000); 
</script></body></html>
)rawliteral";

// ---------- TELEGRAM HANDLER ----------
void handleTelegram() {
  int num = bot->getUpdates(bot->last_message_received + 1);
  while(num) {
    for (int i=0; i<num; i++) {
      String cid = bot->messages[i].chat_id;
      String txt = bot->messages[i].text;
      String fromName = bot->messages[i].from_name;
      
      logEvent("TG_CMD [" + txt + "] User: " + fromName);
      
      if (txt == "/start") {
        bot->sendMessage(cid, "Welcome to *KeyVolution*\n`/status` - Check lock state\n`/login <PASS>` - Admin login", "Markdown");
      }
      else if (txt == "/status") {
        String statStr = (doorState == LOCKED) ? "🔒 LOCKED\n" : "🔓 UNLOCKED\n";
        statStr += overrideActive() ? "🛑 Override: ACTIVE" : "✅ Override: OFF";
        bot->sendMessage(cid, statStr, "");
      }
      else if (txt.startsWith("/login ")) {
        if (txt.substring(7) == devicePass) { botAuth = true; bot->sendMessage(cid, "✅ Logged In! Send `/helpowner`.", ""); } 
        else { bot->sendMessage(cid, "❌ Invalid Password.", ""); }
      } 
      else if (botAuth) {
        if (txt == "/helpowner") {
          String help = "👑 *Admin*\n🔓 `/unlock`\n🔒 `/lock`\n📜 `/history`\n🔑 `/listkeys`\n📡 `/scanner` - Check Box 1 battery/status\n➕ `/addkey <UID> <Name> <master/temp>`\n➖ `/revokekey <UID>`\n🔄 `/reboot`";
          bot->sendMessage(cid, help, "Markdown");
        }
        else if (txt == "/scanner") {
          EspNowMsg pingMsg; pingMsg.type = MSG_PING;
          esp_now_send(box1Mac, (uint8_t*)&pingMsg, sizeof(pingMsg));
          delay(150); 
          if (scannerAwake) bot->sendMessage(cid, "📡 *Scanner Status:* ONLINE & AWAKE", "Markdown");
          else bot->sendMessage(cid, "⚠️ *Scanner Status:* OFFLINE (Or deep-sleeping to save battery)", "Markdown");
        }
        else if (txt == "/unlock") { unlockDoor("Telegram"); bot->sendMessage(cid, "🔓 Unlocked.", ""); }
        else if (txt == "/lock") { lockDoor("Telegram"); bot->sendMessage(cid, "🔒 Locked.", ""); }
        else if (txt == "/reboot") { bot->sendMessage(cid, "🔄 Rebooting...", ""); delay(1000); ESP.restart(); }
        else if (txt == "/history") { 
          File f = LittleFS.open("/history.txt", "r");
          if (!f) {
            bot->sendMessage(cid, "📜 No history found.", "");
          } else {
            std::vector<String> lines;
            while (f.available()) {
              String line = f.readStringUntil('\n');
              line.trim();
              if (line.length() > 0) lines.push_back(line);
            }
            f.close();

            String res = "📜 *Latest 50 Events:*\n```\n";
            int start = (lines.size() > 50) ? (lines.size() - 50) : 0;
            for (int j = start; j < lines.size(); j++) {
              res += lines[j] + "\n";
            }
            res += "```";
            
            bot->sendMessage(cid, res, "Markdown");
          }
        }
        else if (txt == "/listkeys") { 
          if (keyCache.empty()) bot->sendMessage(cid, "No keys.", "");
          else {
            String res = "🔑 *Keys:*\n";
            for (auto const& k : keyCache) res += "👤 " + k.second.name + " (`" + k.first + "`)\n";
            bot->sendMessage(cid, res, "Markdown");
          }
        }
        else if (txt.startsWith("/addkey ")) {
          int firstSpace = txt.indexOf(' ', 8);
          int secondSpace = txt.indexOf(' ', firstSpace + 1);
          
          if (firstSpace == -1 || secondSpace == -1) {
            bot->sendMessage(cid, "❌ Format: `/addkey <UID> <Name> <master/temp>`\nExample: `/addkey 1A2B3C4D5E John master`", "Markdown");
          } else {
            String uid = txt.substring(8, firstSpace);
            String name = txt.substring(firstSpace + 1, secondSpace);
            String type = txt.substring(secondSpace + 1);
            
            File f = LittleFS.open("/keys.json", "r"); 
            StaticJsonDocument<2048> doc; 
            if(f){ deserializeJson(doc, f); f.close(); }
            
            JsonObject obj = doc.createNestedObject(uid); 
            obj["name"] = name; 
            obj["type"] = type;
            
            File f2 = LittleFS.open("/keys.json", "w"); 
            serializeJson(doc, f2); 
            f2.close();
            
            loadKeysToRAM(); 
            logEvent("Telegram: Key Added: " + name);
            bot->sendMessage(cid, "✅ Key added successfully!", "");
          }
        }
        else if (txt.startsWith("/revokekey ")) {
          String uid = txt.substring(11);
          uid.trim();
          
          File f = LittleFS.open("/keys.json", "r"); 
          StaticJsonDocument<2048> doc; 
          if(f){ deserializeJson(doc, f); f.close(); }
          
          if (doc.containsKey(uid)) {
            doc.remove(uid); 
            File f2 = LittleFS.open("/keys.json", "w"); 
            serializeJson(doc, f2); 
            f2.close();
            
            loadKeysToRAM(); 
            logEvent("Telegram: Key Revoked: " + uid);
            bot->sendMessage(cid, "✅ Key revoked successfully!", "");
          } else {
            bot->sendMessage(cid, "❌ Key UID not found.", "");
          }
        }
        else  {
          bot->sendMessage(cid, "Sorry i didnt get that. Use /helpowner for list of commands", "");
        }
      } else { bot->sendMessage(cid, "🔒 Unauthorized. `/login <PASS>`", "Markdown"); }
    }
    num = bot->getUpdates(bot->last_message_received + 1);
  }
}

// ---------- SETUP ----------
void setup() {
  Serial.begin(115200); LittleFS.begin(true); loadConfig(); loadKeysToRAM();
  
  pinMode(SOLENOID, OUTPUT); pinMode(TOUCH, INPUT); pinMode(OVERRIDE, INPUT_PULLUP); 
  pinMode(RESET_BTN, INPUT_PULLUP); // Add this line!
  pinMode(LED_RED, OUTPUT); pinMode(LED_GREEN, OUTPUT);
  
  
  WiFi.mode(WIFI_AP_STA); WiFi.softAP("KeyVolution_Setup", "12345678");
  if (currentSSID.length() > 0) WiFi.begin(currentSSID.c_str(), currentWIFIPASS.c_str());
  secured_client.setInsecure();
  if (botToken.length() > 10) bot = new UniversalTelegramBot(botToken, secured_client); 
  configTime(timezoneOffset * 3600, 0, "pool.ntp.org", "time.nist.gov");

  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onDataRecv);
    esp_now_register_send_cb(OnDataSent); 
    esp_now_peer_info_t peer = {}; memcpy(peer.peer_addr, box1Mac, 6); peer.channel = 0; peer.encrypt = false;
    esp_now_add_peer(&peer);
  }

  // --- WEB ROUTES ---
  server.on("/", [](){ if(!checkAuth()) return; server.send(200, "text/html", index_html); });
  server.on("/statusJSON", [](){ 
    if(!checkAuth()) return;
    String json = "{\"door\":\"" + String(doorState == LOCKED ? "LOCKED" : "UNLOCKED") + "\",\"override\":" + String(overrideActive() ? "true" : "false") + "}";
    server.send(200, "application/json", json);
  });
  server.on("/history", [](){ if(!checkAuth()) return; File f = LittleFS.open("/history.txt", "r"); server.streamFile(f, "text/plain"); f.close(); });
  server.on("/listKeys", [](){ 
    if(!checkAuth()) return;
    StaticJsonDocument<2048> doc;
    for(auto const& k : keyCache) { JsonObject obj = doc.createNestedObject(k.first); obj["name"] = k.second.name; obj["type"] = k.second.type; }
    String res; serializeJson(doc, res); server.send(200, "application/json", res); 
  });
  
  server.on("/pingScanner", HTTP_POST, [](){
    if(!checkAuth()) return;
    EspNowMsg pingMsg; pingMsg.type = MSG_PING;
    esp_now_send(box1Mac, (uint8_t*)&pingMsg, sizeof(pingMsg));
    delay(150); 
    String res = scannerAwake ? "✅ Scanner is ONLINE and responding." : "⚠️ Scanner is OFFLINE (or in Deep Sleep mode to save battery). Swipe a card to wake it up.";
    server.send(200, "text/plain", res);
  });
  server.on("/setLogLimit", HTTP_POST, [](){
    if(!checkAuth()) return;
    if(server.hasArg("l")) { 
      maxLogLines = server.arg("l").toInt(); 
      saveConfig(); 
      logEvent("WebUI: Log limit updated to " + String(maxLogLines)); // Added Log
      server.send(200, "text/plain", "Log limit updated to " + String(maxLogLines)); 
    }
  });

  server.on("/setMelody", HTTP_POST, [](){
    if(!checkAuth()) return;
    if(server.hasArg("m")) { 
      currentMelody = server.arg("m"); 
      saveConfig(); 
      logEvent("WebUI: Success Melody updated."); // Added Log
      server.send(200, "text/plain", "Melody updated."); 
    }
  });

  server.on("/setTimer", HTTP_POST, [](){
    if(!checkAuth()) return;
    if(server.hasArg("t")) { 
      autoLockMS = server.arg("t").toInt(); 
      saveConfig(); 
      logEvent("WebUI: Auto-lock timer changed to " + String(autoLockMS) + "ms"); // Added Log
      server.send(200, "text/plain", "Auto-lock timer updated."); 
    }
  });

  server.on("/setSound", HTTP_POST, [](){
    if(!checkAuth()) return;
    if(server.hasArg("mu")) { 
      isMuted = (server.arg("mu").toInt() == 1); 
      saveConfig(); 
      logEvent(isMuted ? "WebUI: Sound Muted." : "WebUI: Sound Enabled."); // Added Log
      server.send(200, "text/plain", isMuted ? "Sound Muted." : "Sound Enabled."); 
    }
  });

  server.on("/setTZ", HTTP_POST, [](){
    if(!checkAuth()) return;
    if(server.hasArg("z")) { 
      timezoneOffset = server.arg("z").toInt(); 
      saveConfig(); 
      configTime(timezoneOffset * 3600, 0, "pool.ntp.org", "time.nist.gov");
      logEvent("WebUI: Timezone updated to UTC+" + String(timezoneOffset)); // Added Log
      server.send(200, "text/plain", "Timezone updated."); 
    }
  });

  server.on("/setWiFi", HTTP_POST, [](){
    if(!checkAuth()) return;
    if(server.hasArg("s") && server.hasArg("p")) { 
      currentSSID = server.arg("s"); 
      currentWIFIPASS = server.arg("p"); 
      saveConfig(); 
      logEvent("WebUI: WiFi Credentials updated. Rebooting..."); // Added Log
      server.send(200, "text/plain", "WiFi Credentials Saved. Rebooting to connect..."); 
      delay(1000); 
      ESP.restart(); 
    }
  });

  server.on("/setCreds", HTTP_POST, [](){
    if(!checkAuth()) return;
    if(server.hasArg("p")) { 
      devicePass = server.arg("p"); 
      saveConfig(); 
      logEvent("WebUI: Admin password updated."); // Added Log
      server.send(200, "text/plain", "Admin password updated. Please log in again."); 
    }
  });

  server.on("/setToken", HTTP_POST, [](){
    botToken = server.arg("t");
    saveConfig();
    logEvent("WebUI: Telegram Token updated. Rebooting..."); // Added Log
    server.send(200, "text/plain", "Token Updated. Rebooting...");
    delay(500);
    ESP.restart();
  });
  server.on("/addKey", HTTP_POST, [](){
    if(!checkAuth()) return;
    File f = LittleFS.open("/keys.json", "r"); StaticJsonDocument<2048> doc; if(f){ deserializeJson(doc, f); f.close(); }
    JsonObject obj = doc.createNestedObject(server.arg("u")); obj["name"] = server.arg("n"); obj["type"] = server.arg("t");
    File f2 = LittleFS.open("/keys.json", "w"); serializeJson(doc, f2); f2.close();
    loadKeysToRAM(); logEvent("Key Added: " + String(server.arg("n")));
    server.send(200, "text/plain", "Key added!");
  });

  server.on("/revoke", HTTP_POST, [](){
    if(!checkAuth()) return;
    if(server.hasArg("u")) {
      String uid = server.arg("u");
      File f = LittleFS.open("/keys.json", "r"); 
      StaticJsonDocument<2048> doc; 
      if(f){ deserializeJson(doc, f); f.close(); }
      
      doc.remove(uid); 
      File f2 = LittleFS.open("/keys.json", "w"); 
      serializeJson(doc, f2); 
      f2.close();
      
      loadKeysToRAM(); 
      logEvent("Key Revoked: " + uid);
      server.send(200, "text/plain", "Key Revoked successfully.");
    }
  });

  server.on("/factoryReset", HTTP_POST, [](){
    if(!checkAuth()) return;
    LittleFS.format(); 
    server.send(200, "text/plain", "Factory Reset Complete. Rebooting...");
    delay(1000); 
    ESP.restart();
  });

  server.on("/ota", HTTP_GET, []() {
    if(!checkAuth()) return;
    server.send(200, "text/html", "<h2>OTA Firmware Update</h2><form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='update'><br><br><input type='submit' value='Flash Firmware'></form>");
  });

  server.on("/update", HTTP_POST, []() {
    server.send(200, "text/plain", (Update.hasError()) ? "OTA Update Failed." : "OTA Success! Rebooting...");
    delay(1000); ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) { logEvent("OTA Update Started..."); Update.begin(UPDATE_SIZE_UNKNOWN); } 
    else if (upload.status == UPLOAD_FILE_WRITE) { Update.write(upload.buf, upload.currentSize); } 
    else if (upload.status == UPLOAD_FILE_END) { Update.end(true); logEvent("OTA Update Finished."); }
  });

  server.on("/reboot", HTTP_POST, [](){ if(checkAuth()){ server.send(200, "text/plain", "Rebooting..."); delay(500); ESP.restart(); }});
  
  server.begin(); lockDoor("Boot");
}

// ---------- LOOP ----------
void loop() {
  server.handleClient(); handleMelody(); handleWiFi();   
  if (bot != nullptr && millis() - lastBotCheck > 2000) { handleTelegram(); lastBotCheck = millis(); }
  // --- DEDICATED HARD RESET BUTTON (10 SECONDS) ---
  if (digitalRead(RESET_BTN) == LOW) { // LOW means pressed (due to INPUT_PULLUP)
    Serial.println("Reset button held. Starting 10-second countdown...");
    
    unsigned long resetStart = millis();
    unsigned long lastBeepTime = 0;
    bool resetConfirmed = true;
    
    while (millis() - resetStart < 10000) {
      // If the user lets go of the button, abort the reset
      if (digitalRead(RESET_BTN) == HIGH) {
        resetConfirmed = false;
        Serial.println("Hard reset aborted. Returning to normal...");
        noTone(BUZZ);
        break; 
      }
      
      unsigned long elapsed = millis() - resetStart;
      
      // Accelerating beeps
      int beepInterval = map(elapsed, 0, 10000, 800, 60); 
      if (millis() - lastBeepTime > beepInterval) {
        tone(BUZZ, 1000 + (elapsed / 10), beepInterval / 2);
        lastBeepTime = millis();
      }
      
      delay(10); // Keep watchdog timer happy
    }

    if (resetConfirmed) {
      Serial.println("10 SECONDS REACHED. HARD RESET TRIGGERED!");
      tone(BUZZ, 2000, 2000); 
      LittleFS.format();      
      delay(2500);            
      ESP.restart();          
    }
  }
  // --- AUTO LOCK LOGIC ---
  if (doorState == UNLOCKED && !overrideActive()) {
    if (millis() - unlockTime > autoLockMS) {
      lockDoor("Auto-Timer");
    }
  }
  // --- IMPROVED TOUCH SENSOR LOGIC (Debounced) ---
// --- CLEANED UP TOUCH SENSOR LOGIC ---
  bool physicalTouch = (digitalRead(TOUCH) == HIGH);
  static unsigned long lastPhysicalHigh = 0;
  
  if (physicalTouch) {
    lastPhysicalHigh = millis();
    if (!isTouching && (millis() - lastTouchTime > 500)) {
      isTouching = true;
      touchStartTime = millis();
    }
  }

  if (isTouching) {
    unsigned long holdTime = millis() - touchStartTime;

    // RELEASE LOGIC
    // If physical touch is gone for > 200ms (debounce grace period)
    if (!physicalTouch && (millis() - lastPhysicalHigh > 200)) {
      
      // Normal Click: Toggle Door (Only if held for less than 2 seconds)
      if (holdTime < 2000) {
        startMelody("1500,50");
        if (doorState == LOCKED) unlockDoor("Touch"); else lockDoor("Touch");
      } else {
        // If they hold it for 5-10 seconds while running, it just does nothing.
        logEvent("Touch ignored (held too long for normal unlock).");
      }
      
      isTouching = false;
      lastTouchTime = millis();
    }
  }
}