KeyVolution Smart Lock (ESP32)
Overview
KeyVolution is an advanced, ESP32-based smart lock controller. It bridges local, physical access control (via RFID and capacitive touch) with cloud-based remote management. It features a local Web UI for configuration, an integrated Telegram Bot for remote control and notifications, and an ESP-NOW compatibility layer to communicate securely with an external RFID scanner node ("Box 1").
Key Features
Multi-Modal Access: Open the door via physical touch sensor, RFID scanner (ESP-NOW), Web UI, or Telegram Bot.
Telegram Integration: Securely chat with your door. Get instant notifications, check status, remotely lock/unlock, and manage user keys via Telegram commands.
Standalone Web UI: A mobile-friendly dashboard hosted directly on the ESP32 for managing network settings, logs, auto-lock timers, and registered keys.
ESP-NOW Secure Link: Communicates instantly and securely with an external RFID scanner unit without requiring router access.
Non-Volatile Storage: Uses LittleFS to save configurations, logs, and registered key caches, surviving reboots and power outages.
Over-The-Air (OTA) Updates: Update the lock's firmware remotely via the Web UI without needing to plug it into a computer.
Robust Hardware Safety: Includes a physical override switch, dedicated hard-reset button, and debounced touch sensor logic.
Hardware Requirements
Core Components
ESP32 Development Board (NodeMCU or similar)
Solenoid Lock (Requires appropriate power supply, relay/MOSFET, and flyback diode)
Capacitive Touch Sensor (e.g., TTP223)
Active/Passive Buzzer
LEDs (Red for Locked, Green for Unlocked)
Push Buttons / Switches (For hard reset and manual override)
Secondary ESP32/ESP8266 ("Box 1") acting as an RFID scanner over ESP-NOW.
Pinout Configuration
Component
ESP32 Pin
Type
Description
Solenoid Relay
GPIO 26
OUTPUT
Triggers the physical lock mechanism
Buzzer
GPIO 25
OUTPUT
Audio feedback and melodies
Touch Sensor
GPIO 32
INPUT
Capacitive touch to unlock/lock
Override Switch
GPIO 33
INPUT_PULLUP
Physical switch to disable auto-lock
Reset Button
GPIO 2
INPUT_PULLUP
Hold for 10s to format memory and factory reset
Red LED
GPIO 16
OUTPUT
Indicates Locked status
Green LED
GPIO 17
OUTPUT
Indicates Unlocked status

Software Dependencies
Install the following libraries via the Arduino IDE Library Manager before compiling:
ArduinoJson (by Benoit Blanchon) - For parsing config and key files.
UniversalTelegramBot (by Brian Lough) - For the Telegram bot interface.
Standard ESP32 Core Libraries included with the board package: WiFi.h, esp_now.h, WebServer.h, LittleFS.h, WiFiClientSecure.h, Update.h.
Installation & Setup
1. Initial Firmware Flash
Open the project in the Arduino IDE.
Select your specific ESP32 board and COM port.
Click Upload.
2. First-Time Network Configuration
When powered on for the first time (or after a factory reset), the lock will create an Access Point (AP).
Connect to the WiFi network: KeyVolution_Setup (Password: 12345678).
Navigate to http://192.168.4.1 in your browser.
Log in using the default credentials (Username: admin, Password: Admin123).
Scroll to Network Settings, enter your home WiFi SSID and Password, and click Save WiFi. The board will reboot and connect to your home network.
3. Telegram Bot Setup
Open Telegram and message @BotFather to create a new bot.
Copy the HTTP API Token provided by BotFather.
Access the lock's Web UI on your home network.
Paste the Token into the Telegram Setup section and save.
User Guide
Web UI Interface
The web dashboard is the central hub for the lock. It provides:
Real-time Status: View lock state and override switch status.
Keys Management: Add new RFID UIDs, assign names, grant Master/Temp privileges, or revoke access.
Settings: Configure auto-lock timers, timezone offsets, audio muting, and select custom unlock melodies.
History: View the latest 150 access logs and system events.
Telegram Commands
Once logged in via the bot, you can send the following commands:
Command
Description
/start
Welcome message and basic instructions
/login <pass>
Authenticate with the bot (Required first step)
/status
Check the physical state of the lock and override switch
/unlock
Remotely unlocks the door
/lock
Remotely locks the door
/history
Fetches the last 50 system events
/listkeys
Shows all registered user keys and their UIDs
/addkey <UID> <Name> <Type>
Adds a new RFID key (e.g., /addkey 1A2B John master)
/revokekey <UID>
Removes a key from the system
/scanner
Pings the external ESP-NOW scanner to check battery/online status
/reboot
Safely restarts the ESP32

Physical Interactions
Normal Touch: Tap the touch sensor for < 2 seconds to toggle the door (Unlock/Lock).
Hard Reset: If you are locked out or moving the lock to a new network, press and hold the physical Reset button (Pin 2). You will hear accelerating beeps. Hold for 10 seconds until a solid tone plays to format LittleFS and restore factory settings.
Override Switch: Flipping the override switch prevents the door from auto-locking and blocks non-master RFID cards from changing the door state.
Architecture Details
ESP-NOW Communication Protocol
The system uses a custom packed struct to communicate with "Box 1" (the scanner unit).
Data Payload: 1 byte for Message Type, 10 bytes for UID.
Message Types:
1 - Doorbell Pressed.
2 - RFID Card Scanned.
10 - Server Response: RFID OK.
11 - Server Response: RFID Blocked (Override active).
12 - Server Response: RFID Denied (Unregistered).
98 - Ping Scanner.
99 - Sync WiFi Channel (Ensures ESP-NOW stays connected if the home router shifts channels).
