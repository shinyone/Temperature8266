/*
 Temperature
*/

#include "FS.h"               // SPIFFS for store config
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>      // mDNS for ESP8266
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <PubSubClient.h>
#include <DNSServer.h>        // DNS for captive portal
#include <ESP8266WebServer.h> 
ESP8266WebServer server(80);
#include <ArduinoOTA.h>   // for OTA
#include <DHTesp.h>

#include "config.h"       // config file
#include "html_common.h"  // common code HTML (like header, footer)
#include "javascript_common.h"  // common code javascript (like refresh page)
#include "html_init.h"    // code html for initial config
#include "html_menu.h"    // code html for menu
#include "html_pages.h"   // code html for pages
#include "forced_settings.h"; // prefilled values
boolean forceSettings = false;

// Temperature sensor
#define DHT_TYPE DHT22

// Languages
#ifndef MY_LANGUAGE
  #include "languages/en-GB.h" // default language English
#else
  #define QUOTEME(x) QUOTEME_1(x)
  #define QUOTEME_1(x) #x
  #define INCLUDE_FILE(x) QUOTEME(languages/x.h)
  #include INCLUDE_FILE(MY_LANGUAGE)
#endif

X509List cert(TELEGRAM_CERTIFICATE_ROOT);
WiFiClientSecure secured_client;
UniversalTelegramBot *bot;

unsigned long lastMqttRetry;
unsigned long lastHeartbeatSent;

WiFiClient espClient;
PubSubClient mqtt_client(espClient);
DHTesp dht;

//Captive portal variables, only used for config page
const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 4, 1);
IPAddress netMsk(255, 255, 255, 0);
DNSServer dnsServer;

boolean debugging = false;
boolean captive = false;
boolean mqtt_config = false;
boolean wifi_config = false;
boolean temperature_config = false;

//Web OTA
int uploaderror = 0;

inline int32_t timeDiff(const unsigned long prev, const unsigned long next) {
    return ((int32_t)(next - prev));
}

inline long timePassedSince(unsigned long timestamp) {
    return timeDiff(timestamp, millis());
}

void setup() {
  // Start serial for debug
  Serial.begin(115200);
      
  // Mount SPIFFS filesystem
  if (SPIFFS.begin())
  {
    if (debugging) Serial.println(F("Mounted file system"));
  }
  else
  {
    if (debugging) Serial.println(F("Failed to mount FS -> formating"));
    SPIFFS.format();
    if (debugging) Serial.println(F("Mounted file system after formating"));
  }
  //set led pin as output
  pinMode(blueLedPin, OUTPUT);

  if (SPIFFS.exists(console_file)) {
    SPIFFS.remove(console_file);
  }

  Serial.println("Starting " + program_name);
  write_log("Starting " + program_name);

  // initialize defaults
  setDefaults();

  if (forceSettings) {
    if (debugging) Serial.println(F("Forcing settings"));
    saveWifi(WIFI_SSID, WIFI_PASSWORD, WIFI_HOST_NAME, OTA_PASSWORD);
    saveMqtt(MQTT_FRIENDLY_NAME, MQTT_SERVER_IP, MQTT_SERVER_PORT, MQTT_USER, MQTT_PASS, MQTT_TOPIC);
    saveTelegram(TELEGRAM_CHAT_ID, TELEGRAM_BOT_TOKEN);
    saveTemperature("SENSOR_P3", "22", "RELAY_P5");
    saveOthers(ON, HOME_ASSISTANT_TOPIC, ON);
  }
  
  // load other settings 
  if (!loadOthers()) {
    // defaults if not loaded 
    others_haad = true;
    others_haa_topic = "homeassistant";
  }

  //Define default hostname
  hostname += program_name;
  hostname += "_" + getId();
  mqtt_client_id = hostname;
  WiFi.hostname(hostname.c_str());

  // load wifi settings
  wifi_config_exists = loadWifi();

  // load telegram parameters
  telegram_connected = loadTelegram();
  if (telegram_connected) {
    bot = new UniversalTelegramBot(telegram_bot_token, secured_client);
  }

  // load Temperature parameters
  temperature_connected = false;
  if (loadTemperature()) {
    pinMode(temperature_sensor_pin, INPUT);
    pinMode(temperature_relay_pin, OUTPUT);
    digitalWrite(temperature_relay_pin, HIGH);
    power_status = false;
    dht.setup(temperature_sensor_pin, DHTesp::DHT22);
  }

  if (initWifi()) {
    //Web interface
    server.on("/", handleRoot);
    server.on("/setup", handleSetup);
    server.on("/mqtt", handleMqtt);
    server.on("/wifi", handleWifi);
    server.on("/telegram", handleTelegram);
    server.on("/temperature", handleTemperature);
    server.on("/others", handleOthers);
    server.on("/status", handleStatus);

    server.onNotFound(handleNotFound);
    if (login_password.length() > 0) {
      server.on("/login", handleLogin);
      //here the list of headers to be recorded, use for authentication
      const char * headerkeys[] = {"User-Agent", "Cookie"} ;
      size_t headerkeyssize = sizeof(headerkeys) / sizeof(char*);
      //ask server to track these headers
      server.collectHeaders(headerkeys, headerkeyssize);
    }
    server.on("/upgrade", handleUpgrade);
    server.on("/upload", HTTP_POST, handleUploadDone, handleUploadLoop);

    // time setup
    configTime(0, 0, "pool.ntp.org"); // get UTC time via NTP
    time_t now = time(nullptr);
    // ensure current time not Jan 01 1970 (take a while to connect)
    while (now < 24 * 3600)
    {
        delay(100);
        now = time(nullptr);
    }

    if (telegram_connected) {
      bot->sendMessage(telegram_chat_id, program_name + ": starting ...", "");
    }
    lastHeartbeatSent = 0;

    server.begin();
    lastMqttRetry = 0;
    
    if (loadMqtt()) {
      // if (debugOutput) write_log("Starting MQTT");

      // There is/can be a problem loading the settings
      if (mqtt_fn.equals("null")) {
        mqtt_fn = String(MQTT_FRIENDLY_NAME);
      }

      // heartbeat topic
      mqtt_heartbeat_topic = mqtt_topic + "/" + mqtt_fn + "/heartbeat";

      // home assistant topics
      ha_command_topic = mqtt_topic + "/" + mqtt_fn + "/set";
      ha_state_topic = mqtt_topic + "/" + mqtt_fn + "/state";

      if (others_haad) {
        // discovery       
        ha_config_topic = others_haa_topic + "/switch/" + mqtt_fn + "/config";       
      }
     
      // startup mqtt connection
      initMqtt();
    }
    else {
      write_log("Not found MQTT config go to configuration page");
    }
  }
  else {
    dnsServer.start(DNS_PORT, "*", apIP);
    initCaptivePortal();
  }
  initOTA();
}


// SAVE

void saveJson(DynamicJsonDocument& doc, const char* fileName, String configName) {
  File configFile = SPIFFS.open(fileName, "w");
  if (!configFile) {
    if (debugging) Serial.println("Failed to open " + configName + " config file for writing");
    // if (debugOutput) write_log("Failed to open " + configName + " config file for writing");
  }
  
  serializeJson(doc, configFile);

  if (debugging)  { String result = configName + " settings saved: ";  serializeJson(doc, result); Serial.println(result); }
  write_log(configName + " settings saved"); 
  configFile.close();
}

void saveMqtt(String mqttFn, String mqttHost, String mqttPort, String mqttUser,
              String mqttPwd, String mqttTopic) {

  const size_t capacity = JSON_OBJECT_SIZE(7) + 600;
  DynamicJsonDocument doc(capacity);
  // if mqtt port is empty, we use default port
  if (mqttPort[0] == '\0') mqttPort = "1883";
  doc["mqtt_fn"]   = mqttFn;
  doc["mqtt_host"] = mqttHost;
  doc["mqtt_port"] = mqttPort;
  doc["mqtt_user"] = mqttUser;
  doc["mqtt_pwd"] = mqttPwd;
  doc["mqtt_topic"] = mqttTopic;
  saveJson(doc, mqtt_conf, "MQTT");
}


void saveWifi(String apSsid, String apPwd, String hostName, String otaPwd) {
  const size_t capacity = JSON_OBJECT_SIZE(5) + 256;
  DynamicJsonDocument doc(capacity);
  doc["ap_ssid"] = apSsid;
  doc["ap_pwd"] = apPwd;
  doc["hostname"] = hostName;
  doc["ota_pwd"] = otaPwd;
  saveJson(doc, wifi_conf, "Wifi");
}


void saveTelegram(String tci, String tbt) {
  const size_t capacity = JSON_OBJECT_SIZE(3) + 256;
  DynamicJsonDocument doc(capacity);
  doc["tci"] = tci;
  doc["tbt"] = tbt;
  saveJson(doc, telegram_conf, "Telegram");
}

void saveTemperature(String sensor_pin, String temp_limit, String relay_pin) {
  const size_t capacity = JSON_OBJECT_SIZE(4) + 256;
  DynamicJsonDocument doc(capacity);
  doc["sensor_pin"] = sensor_pin;
  doc["temp_limit"] = temp_limit;
  doc["relay_pin"] = relay_pin;
  saveJson(doc, temperature_conf, "Temperature");  
}

void saveOthers(String haad, String haat, String debug) {
  const size_t capacity = JSON_OBJECT_SIZE(4) + 256;
  DynamicJsonDocument doc(capacity);
  doc["haad"] = haad;
  doc["haat"] = haat;
  doc["debug"] = debug;
  saveJson(doc, others_conf, "Others");    
}


// Initialize captive portal page
void initCaptivePortal() {
  if (debugging) Serial.println(F("Starting captive portal"));
  write_log("Starting captive portal");

  server.on("/", handleInitSetup);
  server.on("/save", handleSaveWifi);
  server.on("/reboot", handleReboot);
  server.onNotFound(handleNotFound);
  server.begin();
  captive = true;
}

void initMqtt() {
  mqtt_client.setServer(mqtt_server.c_str(), atoi(mqtt_port.c_str()));
  mqtt_client.setCallback(mqttCallback);
  mqttConnect();
}

// Enable OTA only when connected as a client.
void initOTA() {
  //write_log("Start OTA Listener");
  ArduinoOTA.setHostname(hostname.c_str());
  if (ota_pwd.length() > 0) {
    ArduinoOTA.setPassword(ota_pwd.c_str());
  }
  ArduinoOTA.onStart([]() {
    //write_log("Start");
  });
  ArduinoOTA.onEnd([]() {
    //write_log("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    //    write_log("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    //    write_log("Error[%u]: ", error);
    // if (error == OTA_AUTH_ERROR) if (debugging) Serial.println(F("Auth Failed"));
    // else if (error == OTA_BEGIN_ERROR) if (debugging) Serial.println(F("Begin Failed"));
    // else if (error == OTA_CONNECT_ERROR) if (debugging) Serial.println(F("Connect Failed"));
    // else if (error == OTA_RECEIVE_ERROR) if (debugging) Serial.println(F("Receive Failed"));
    // else if (error == OTA_END_ERROR) if (debugging) Serial.println(F("End Failed"));
  });
  ArduinoOTA.begin();
}

// LOAD

DynamicJsonDocument loadJson(const char* fileName, String configName) {
  const size_t capacity = JSON_OBJECT_SIZE(8) + 600;
  DynamicJsonDocument doc(capacity);
  
  if (!SPIFFS.exists(fileName)) {
    if (debugging) Serial.println(configName + " config file does not exist!");
    // if (debugOutput) write_log(configName + " config file does not exist!");      
    return doc;
  }
  File configFile = SPIFFS.open(fileName, "r");
  if (!configFile) {
    if (debugging) Serial.println("Failed to open " + configName + " config file");
    // if (debugOutput) write_log("Failed to open " + configName + " config file");        
    return doc;
  }
  size_t size = configFile.size();
  if (size > 1024) {
    if (debugging) Serial.println(configName + " config file size is too large");
    // if (debugOutput) write_log(configName + " config file size is too large");  
    return doc;
  }

  // Allocate a buffer to store contents of the file.
  std::unique_ptr<char[]> buf(new char[size]);
  configFile.readBytes(buf.get(), size);
  DeserializationError error = deserializeJson(doc, buf.get());
  
  // get json document
  if (error != DeserializationError::Ok) {
      Serial.println(configName + " deserializeJson failed " + error.c_str());
      write_log(configName + " deserializeJson failed " + error.c_str());  
  }
  configFile.close();

  if (debugging)  { String result = configName + " contents: ";  serializeJson(doc, result); Serial.println(result); }
  
  return doc;
}

bool loadWifi() {
  DynamicJsonDocument doc = loadJson(wifi_conf, "Wifi");
  if (doc.isNull()) { 
    write_log("No existing wifi settings found"); 
    return false;
  }

  hostname = doc["hostname"].as<String>();
  ap_ssid  = doc["ap_ssid"].as<String>();
  ap_pwd   = doc["ap_pwd"].as<String>();
  //prevent ota password is "null" if not exist key
  if (doc.containsKey("ota_pwd")) {
    ota_pwd  = doc["ota_pwd"].as<String>();
  } else {
    ota_pwd = "";
  }
  
  return true;
}

bool loadMqtt() { 
  DynamicJsonDocument doc = loadJson(mqtt_conf, "MQTT");
  if (doc.isNull()) { 
    write_log("No existing MQTT settings found"); 
    return false;
  }

  mqtt_fn             = doc["mqtt_fn"].as<String>();
  mqtt_server         = doc["mqtt_host"].as<String>();
  mqtt_port           = doc["mqtt_port"].as<String>();
  mqtt_username       = doc["mqtt_user"].as<String>();
  mqtt_password       = doc["mqtt_pwd"].as<String>();
  mqtt_topic          = doc["mqtt_topic"].as<String>();

  mqtt_config = true;
  return true;
}


bool loadTelegram() {
  DynamicJsonDocument doc = loadJson(telegram_conf, "Telegram");
  if (doc.isNull()) { 
    write_log("No existing Telegram settings found"); 
    return false;
  }

  telegram_chat_id = doc["tci"].as<String>();
  telegram_bot_token = doc["tbt"].as<String>();

  return true;
}


bool loadTemperature() {
  DynamicJsonDocument doc = loadJson(temperature_conf, "Temperature");
  if (doc.isNull()) { 
    write_log("No existing Temperature settings found"); 
    return false;
  }
  
  String sensor_pin = doc["sensor_pin"].as<String>();

  if (strcmp(sensor_pin.c_str(), "SENSOR_P0") == 0) {
    temperature_sensor_pin = D0;
  } else if (strcmp(sensor_pin.c_str(), "SENSOR_P1") == 0) {
    temperature_sensor_pin = D1;
  } else if (strcmp(sensor_pin.c_str(), "SENSOR_P2") == 0) {
    temperature_sensor_pin = D2;
  } else if (strcmp(sensor_pin.c_str(), "SENSOR_P3") == 0) {
    temperature_sensor_pin = D3;
  } else if (strcmp(sensor_pin.c_str(), "SENSOR_P4") == 0) {
    temperature_sensor_pin = D4;
  } else if (strcmp(sensor_pin.c_str(), "SENSOR_P5") == 0) {
    temperature_sensor_pin = D5;
  } else if (strcmp(sensor_pin.c_str(), "SENSOR_P6") == 0) {
    temperature_sensor_pin = D6;
  } else if (strcmp(sensor_pin.c_str(), "SENSOR_P7") == 0) {
    temperature_sensor_pin = D7;
  } else if (strcmp(sensor_pin.c_str(), "SENSOR_P8") == 0) {
    temperature_sensor_pin = D8;
  } else if (strcmp(sensor_pin.c_str(), "SENSOR_TX") == 0) {
    temperature_sensor_pin = TX;
  } else if (strcmp(sensor_pin.c_str(), "SENSOR_RX") == 0) {
    temperature_sensor_pin = RX;
  }

  temperature_limit = doc["temp_limit"].as<float>();
  
  String relay_pin = doc["relay_pin"].as<String>();

  if (strcmp(relay_pin.c_str(), "RELAY_P0") == 0) {
    temperature_relay_pin = D0;
  } else if (strcmp(relay_pin.c_str(), "RELAY_P1") == 0) {
    temperature_relay_pin = D1;
  } else if (strcmp(relay_pin.c_str(), "RELAY_P2") == 0) {
    temperature_relay_pin = D2;
  } else if (strcmp(relay_pin.c_str(), "RELAY_P3") == 0) {
    temperature_relay_pin = D3;
  } else if (strcmp(relay_pin.c_str(), "RELAY_P4") == 0) {
    temperature_relay_pin = D4;
  } else if (strcmp(relay_pin.c_str(), "RELAY_P5") == 0) {
    temperature_relay_pin = D5;
  } else if (strcmp(relay_pin.c_str(), "RELAY_P6") == 0) {
    temperature_relay_pin = D6;
  } else if (strcmp(relay_pin.c_str(), "RELAY_P7") == 0) {
    temperature_relay_pin = D7;
  } else if (strcmp(relay_pin.c_str(), "RELAY_P8") == 0) {
    temperature_relay_pin = D8;
  } else if (strcmp(relay_pin.c_str(), "RELAY_TX") == 0) {
    temperature_relay_pin = TX;
  } else if (strcmp(relay_pin.c_str(), "RELAY_RX") == 0) {
    temperature_relay_pin = RX;
  }

  temperature_config = true;
  return true;
}


bool loadOthers() {
  DynamicJsonDocument doc = loadJson(others_conf, "Others");
  if (doc.isNull()) { 
    write_log("No existing Others settings found"); 
    return false;
  }

  others_haa_topic = doc["haat"].as<String>();
  String haad = doc["haad"].as<String>();
  String debug = doc["debug"].as<String>();

  others_haad = (strcmp(haad.c_str(), "ON") == 0);
  debugging = (strcmp(debug.c_str(), "ON") == 0);

  return true;
}


void setDefaults() {
  ap_ssid = WIFI_SSID;
  ap_pwd  = WIFI_PASSWORD;
  power_status = false;
}

boolean initWifi() {
  bool connectWifiSuccess = true;
  if (ap_ssid[0] != '\0') {
    connectWifiSuccess = wifi_config = connectWifi();
    if (connectWifiSuccess) {
      return true;
    }
    else
    {
      // reset hostname back to default before starting AP mode for privacy
      hostname += program_name;
      hostname += "_" + getId();
    }
  }

  if (debugging) Serial.println(F("\n\r \n\rStarting in AP mode"));
  WiFi.mode(WIFI_AP);
  wifi_timeout = millis() + WIFI_RETRY_INTERVAL_MS;
  WiFi.persistent(false); //fix crash esp32 https://github.com/espressif/arduino-esp32/issues/2025
  if (!connectWifiSuccess and login_password != "") {
    // Set AP password when falling back to AP on fail
    WiFi.softAP(hostname.c_str(), login_password);
  }
  else {
    // First time setup does not require password
    WiFi.softAP(hostname.c_str());
  }
  delay(2000); // VERY IMPORTANT
  WiFi.softAPConfig(apIP, apIP, netMsk);
  if (debugging) Serial.print(F("IP address: "));
  if (debugging) Serial.println(WiFi.softAPIP());
  //ticker.attach(0.2, tick); // Start LED to flash rapidly to indicate we are ready for setting up the wifi-connection (entered captive portal).
  wifi_config = false;
  return false;
}

// Handler webserver response
void sendWrappedHTML(String content) {
  String headerContent = FPSTR(html_common_header);
  String footerContent = FPSTR(html_common_footer);
  String toSend = headerContent + content + footerContent;
  toSend.replace(F("_UNIT_NAME_"), hostname);
  toSend.replace(F("_PROGRAM_NAME_"), program_name);
  toSend.replace(F("_VERSION_"), program_version);
  server.send(200, F("text/html"), toSend);
}

void handleNotFound() {
  if (captive) {
    String initSetupContent = FPSTR(html_init_setup);
    initSetupContent.replace("_TXT_INIT_TITLE_",FPSTR(txt_init_title));
    initSetupContent.replace("_TXT_INIT_HOST_",FPSTR(txt_wifi_hostname));
    initSetupContent.replace("_UNIT_NAME_", hostname);
    initSetupContent.replace("_TXT_INIT_SSID_",FPSTR(txt_wifi_SSID));
    initSetupContent.replace("_TXT_INIT_PSK_",FPSTR(txt_wifi_psk));
    initSetupContent.replace("_TXT_INIT_OTA_",FPSTR(txt_wifi_otap));
    initSetupContent.replace("_TXT_SAVE_",FPSTR(txt_save));
    initSetupContent.replace("_TXT_REBOOT_",FPSTR(txt_reboot));

    server.send(200, "text/html", initSetupContent);
  }
  else {
    server.sendHeader("Location", "/");
    server.sendHeader("Cache-Control", "no-cache");
    server.send(302);
    return;
  }
}

void handleSaveWifi() {
  checkLogin();
  if (server.method() == HTTP_POST) {
    saveWifi(server.arg("ssid"), server.arg("psk"), server.arg("hn"), server.arg("otapwd"));
  }
  String initSavePage =  FPSTR(html_init_save);
  initSavePage.replace("_TXT_INIT_REBOOT_MESS_",FPSTR(txt_init_reboot_mes));
  sendWrappedHTML(initSavePage);
  delay(500);
  ESP.restart();
}

void handleReboot() {
  String initRebootPage = FPSTR(html_init_reboot);
  initRebootPage.replace("_TXT_INIT_REBOOT_",FPSTR(txt_init_reboot));
  sendWrappedHTML(initRebootPage);
  delay(500);
  ESP.restart();
}

void handleRoot() {
  checkLogin();
  if (server.hasArg("REBOOT")) {
    String rebootPage = FPSTR(html_page_reboot);
    String countDown = FPSTR(count_down_script);
    rebootPage.replace("_TXT_M_REBOOT_",FPSTR(txt_m_reboot));
    sendWrappedHTML(rebootPage + countDown);
    delay(500);
    ESP.reset();
  }
  else if (server.hasArg("REFRESH")) {  
    if (server.hasArg("TOGGLE")) {  
      powerToggle();
    }

    server.send(200, F("text/html"), power_status ? FPSTR(txt_on) : FPSTR(txt_off));
  }
  else {
    String menuRootPage = FPSTR(html_menu_root);
    menuRootPage.replace("_SHOW_LOGOUT_", (String)(login_password.length() > 0));

    menuRootPage.replace("_TXT_POWER_TOGGLE_",FPSTR(txt_toggle));
    menuRootPage.replace("_TXT_SETUP_",FPSTR(txt_setup));
    menuRootPage.replace("_TXT_STATUS_",FPSTR(txt_status));
    menuRootPage.replace("_TXT_FW_UPGRADE_",FPSTR(txt_firmware_upgrade));
    menuRootPage.replace("_TXT_REBOOT_",FPSTR(txt_reboot));
    menuRootPage.replace("_TXT_LOGOUT_",FPSTR(txt_logout));
    sendWrappedHTML(menuRootPage);
  }
}

void handleInitSetup() {
  String initSetupPage = FPSTR(html_init_setup);
  initSetupPage.replace("_TXT_INIT_TITLE_",FPSTR(txt_init_title));
  initSetupPage.replace("_TXT_INIT_HOST_",FPSTR(txt_wifi_hostname));
  initSetupPage.replace("_TXT_INIT_SSID_",FPSTR(txt_wifi_SSID));
  initSetupPage.replace("_TXT_INIT_PSK_",FPSTR(txt_wifi_psk));
  initSetupPage.replace("_TXT_INIT_OTA_",FPSTR(txt_wifi_otap));
  initSetupPage.replace("_TXT_SAVE_",FPSTR(txt_save));
  initSetupPage.replace("_TXT_REBOOT_",FPSTR(txt_reboot));

  sendWrappedHTML(initSetupPage);
}

void handleSetup() {
  checkLogin();
  if (server.hasArg("RESET")) {
    String pageReset = FPSTR(html_page_reset);
    String ssid = program_name;
    ssid += "_" + getId();
    pageReset.replace("_TXT_M_RESET_",FPSTR(txt_m_reset));
    pageReset.replace("_SSID_",ssid);
    sendWrappedHTML(pageReset);
    SPIFFS.format();
    delay(500);
    ESP.reset();
  }
  else {
    String menuSetupPage = FPSTR(html_menu_setup);
    menuSetupPage.replace("_TXT_MQTT_",FPSTR(txt_MQTT));
    menuSetupPage.replace("_TXT_WIFI_",FPSTR(txt_WIFI));
    menuSetupPage.replace("_TXT_TELEGRAM_",FPSTR(txt_telegram));
    menuSetupPage.replace("_TXT_TEMPERATURE_",FPSTR(txt_temperature));
    menuSetupPage.replace("_TXT_OTHERS_",FPSTR(txt_others));
    menuSetupPage.replace("_TXT_RESET_",FPSTR(txt_reset));
    menuSetupPage.replace("_TXT_BACK_",FPSTR(txt_back));
    menuSetupPage.replace("_TXT_RESETCONFIRM_",FPSTR(txt_reset_confirm));
    sendWrappedHTML(menuSetupPage);
  }

}

void rebootAndSendPage() {
    String saveRebootPage =  FPSTR(html_page_save_reboot);
    String countDown = FPSTR(count_down_script);
    saveRebootPage.replace("_TXT_M_SAVE_",FPSTR(txt_m_save));
    sendWrappedHTML(saveRebootPage + countDown);
    delay(500);
    ESP.restart();
}


void handleTelegram() {
  checkLogin();
  if (server.method() == HTTP_POST) {
    saveTelegram(server.arg("tci"), server.arg("tbt"));
    rebootAndSendPage();
  }
  else {
    String telegramPage =  FPSTR(html_page_telegram);
    telegramPage.replace("_TXT_SAVE_", FPSTR(txt_save));
    telegramPage.replace("_TXT_BACK_", FPSTR(txt_back));
    telegramPage.replace("_TXT_TELEGRAM_TITLE_", FPSTR(txt_telegram_title));
    telegramPage.replace("_TXT_TELEGRAM_CHAT_ID_", FPSTR(txt_telegram_chat_id));
    telegramPage.replace("_TXT_TELEGRAM_BOT_TOKEN_", FPSTR(txt_telegram_bot_token));

    telegramPage.replace("_TELEGRAM_CHAT_ID_", telegram_chat_id);
    telegramPage.replace("_TELEGRAM_BOT_TOKEN_", telegram_bot_token);
  
    sendWrappedHTML(telegramPage);
  }
}

void handleTemperature() {
  checkLogin();
  if (server.method() == HTTP_POST) {
    saveTemperature(server.arg("sensor_pin"), server.arg("temp_limit"), server.arg("relay_pin"));
    rebootAndSendPage();
  }
  else {
    String temperaturePage =  FPSTR(html_page_temperature);
    temperaturePage.replace("_TXT_SAVE_", FPSTR(txt_save));
    temperaturePage.replace("_TXT_BACK_", FPSTR(txt_back));
    temperaturePage.replace("_TXT_TEMPERATURE_TITLE_", FPSTR(txt_temperature_title));
    
    temperaturePage.replace("_TXT_P0_", FPSTR(txt_pin0));
    temperaturePage.replace("_TXT_P1_", FPSTR(txt_pin1));
    temperaturePage.replace("_TXT_P2_", FPSTR(txt_pin2));
    temperaturePage.replace("_TXT_P3_", FPSTR(txt_pin3));
    temperaturePage.replace("_TXT_P4_", FPSTR(txt_pin4));
    temperaturePage.replace("_TXT_P5_", FPSTR(txt_pin5));
    temperaturePage.replace("_TXT_P6_", FPSTR(txt_pin6));
    temperaturePage.replace("_TXT_P7_", FPSTR(txt_pin7));
    temperaturePage.replace("_TXT_P8_", FPSTR(txt_pin8));
    temperaturePage.replace("_TXT_TX_", FPSTR(txt_pinTX));
    temperaturePage.replace("_TXT_RX_", FPSTR(txt_pinRX));
    
    temperaturePage.replace("_TXT_TEMPERATURE_SENSOR_", txt_temperature_sensor_pin);
    temperaturePage.replace("_TXT_TEMPERATURE_RELAY_", txt_temperature_relay_pin);

    temperaturePage.replace("_TXT_TEMPERATURE_LIMIT_", txt_temperature_limit);
    temperaturePage.replace("_TEMPERATURE_LIMIT_", String(temperature_limit));

    if (temperature_sensor_pin == D0) {
      temperaturePage.replace("_SENSOR_P0_", "selected");
    } else if (temperature_sensor_pin == D1) {
      temperaturePage.replace("_SENSOR_P1_", "selected");
    } else if (temperature_sensor_pin == D2) {
      temperaturePage.replace("_SENSOR_P2_", "selected");
    } else if (temperature_sensor_pin == D3) {
      temperaturePage.replace("_SENSOR_P3_", "selected");
    } else if (temperature_sensor_pin == D4) {
      temperaturePage.replace("_SENSOR_P4_", "selected");
    } else if (temperature_sensor_pin == D5) {
      temperaturePage.replace("_SENSOR_P5_", "selected");
    } else if (temperature_sensor_pin == D6) {
      temperaturePage.replace("_SENSOR_P6_", "selected");
    } else if (temperature_sensor_pin == D7) {
      temperaturePage.replace("_SENSOR_P7_", "selected");
    } else if (temperature_sensor_pin == D8) {
      temperaturePage.replace("_SENSOR_P8_", "selected");
    } else if (temperature_sensor_pin == TX) {
      temperaturePage.replace("_SENSOR_TX_", "selected");
    } else if (temperature_sensor_pin == RX) {
      temperaturePage.replace("_SENSOR_RX_", "selected");
    }
   
    if (temperature_relay_pin == D0) {
      temperaturePage.replace("_RELAY_P0_", "selected");
    } else if (temperature_relay_pin == D1) {
      temperaturePage.replace("_RELAY_P1_", "selected");
    } else if (temperature_relay_pin == D2) {
      temperaturePage.replace("_RELAY_P2_", "selected");
    } else if (temperature_relay_pin == D3) {
      temperaturePage.replace("_RELAY_P3_", "selected");
    } else if (temperature_relay_pin == D4) {
      temperaturePage.replace("_RELAY_P4_", "selected");
    } else if (temperature_relay_pin == D5) {
      temperaturePage.replace("_RELAY_P5_", "selected");
    } else if (temperature_relay_pin == D6) {
      temperaturePage.replace("_RELAY_P6_", "selected");
    } else if (temperature_relay_pin == D7) {
      temperaturePage.replace("_RELAY_P7_", "selected");
    } else if (temperature_relay_pin == D8) {
      temperaturePage.replace("_RELAY_P8_", "selected");
    } else if (temperature_relay_pin == TX) {
      temperaturePage.replace("_RELAY_TX_", "selected");
    } else if (temperature_relay_pin == RX) {
      temperaturePage.replace("_RELAY_RX_", "selected");
    }

    sendWrappedHTML(temperaturePage);
  }
}

void handleOthers() {
  checkLogin();
  if (server.method() == HTTP_POST) {
    saveOthers(server.arg("haad"), server.arg("haat"), server.arg("debug"));
    rebootAndSendPage();
  }
  else {
    String othersPage =  FPSTR(html_page_others);
    othersPage.replace("_TXT_SAVE_", FPSTR(txt_save));
    othersPage.replace("_TXT_BACK_", FPSTR(txt_back));
    othersPage.replace("_TXT_ON_", FPSTR(txt_on));
    othersPage.replace("_TXT_OFF_", FPSTR(txt_off));
    othersPage.replace("_TXT_OTHERS_TITLE_", FPSTR(txt_others_title));
    othersPage.replace("_TXT_OTHERS_HAAUTO_", FPSTR(txt_others_haad));
    othersPage.replace("_TXT_OTHERS_HATOPIC_", FPSTR(txt_others_hatopic));    
    othersPage.replace("_TXT_OTHERS_DEBUG_", FPSTR(txt_others_debug));
    othersPage.replace("_TXT_OTHERS_LOG_", FPSTR(txt_others_log));    
    othersPage.replace("_HAA_TOPIC_", others_haa_topic);
    
    // read log file
    File logFile = SPIFFS.open(console_file, "r");
    String debugLogData = "";   
    while (logFile.available()){
      debugLogData += logFile.readStringUntil('\n') + "<br>";
    }
    logFile.close();
    othersPage.replace("_DEBUG_CONSOLE_", debugLogData);

    if (others_haad) {
      othersPage.replace("_HAAD_ON_", "selected");
    }
    else {
      othersPage.replace("_HAAD_OFF_", "selected");
    }
    if (debugOutput) {
      othersPage.replace("_OTHERS_ON_", "selected");
    }
    else {
      othersPage.replace("_OTHERS_OFF_", "selected");
    }
    
    sendWrappedHTML(othersPage);
  }
}

void handleMqtt() {
  checkLogin();
  if (server.method() == HTTP_POST) {
    saveMqtt(server.arg("fn"), server.arg("mh"), server.arg("ml"), server.arg("mu"), server.arg("mp"), server.arg("mt"));
    rebootAndSendPage();
  }
  else {
    String mqttPage =  FPSTR(html_page_mqtt);
    mqttPage.replace("_TXT_SAVE_", FPSTR(txt_save));
    mqttPage.replace("_TXT_BACK_", FPSTR(txt_back));
    mqttPage.replace("_TXT_MQTT_TITLE_", FPSTR(txt_mqtt_title));
    mqttPage.replace("_TXT_MQTT_FN_", FPSTR(txt_mqtt_fn));
    mqttPage.replace("_TXT_MQTT_HOST_", FPSTR(txt_mqtt_host));
    mqttPage.replace("_TXT_MQTT_PORT_", FPSTR(txt_mqtt_port));
    mqttPage.replace("_TXT_MQTT_USER_", FPSTR(txt_mqtt_user));
    mqttPage.replace("_TXT_MQTT_PASSWORD_", FPSTR(txt_mqtt_password));
    mqttPage.replace("_TXT_MQTT_TOPIC_", FPSTR(txt_mqtt_topic));
    mqttPage.replace(F("_MQTT_FN_"), mqtt_fn);
    mqttPage.replace(F("_MQTT_HOST_"), mqtt_server);
    mqttPage.replace(F("_MQTT_PORT_"), String(mqtt_port));
    mqttPage.replace(F("_MQTT_USER_"), mqtt_username);
    mqttPage.replace(F("_MQTT_PASSWORD_"), mqtt_password);
    mqttPage.replace(F("_MQTT_TOPIC_"), mqtt_topic);
    sendWrappedHTML(mqttPage);
  }
}


void handleWifi() {
  checkLogin();
  if (server.method() == HTTP_POST) {
    saveWifi(server.arg("ssid"), server.arg("psk"), server.arg("hn"), server.arg("otapwd"));
    rebootAndSendPage();
    ESP.reset();
  }
  else {
    String wifiPage =  FPSTR(html_page_wifi);
    wifiPage.replace("_TXT_SAVE_", FPSTR(txt_save));
    wifiPage.replace("_TXT_BACK_", FPSTR(txt_back));
    wifiPage.replace("_TXT_WIFI_TITLE_", FPSTR(txt_wifi_title));
    wifiPage.replace("_TXT_WIFI_HOST_", FPSTR(txt_wifi_hostname));
    wifiPage.replace("_TXT_WIFI_SSID_", FPSTR(txt_wifi_SSID));
    wifiPage.replace("_TXT_WIFI_PSK_", FPSTR(txt_wifi_psk));
    wifiPage.replace("_TXT_WIFI_OTAP_", FPSTR(txt_wifi_otap));
    wifiPage.replace(F("_SSID_"), ap_ssid);
    wifiPage.replace(F("_PSK_"), ap_pwd);
    wifiPage.replace(F("_OTA_PWD_"), ota_pwd);
    sendWrappedHTML(wifiPage);
  }

}

void handleStatus() {
  String statusPage =  FPSTR(html_page_status);
  statusPage.replace("_TXT_BACK_", FPSTR(txt_back));
  statusPage.replace("_TXT_STATUS_TITLE_", FPSTR(txt_status_title));
  statusPage.replace("_TXT_TEMPERATURE_", FPSTR(txt_current_temperature));
  statusPage.replace("_TXT_HUMIDITY_", FPSTR(txt_current_humidity));
  statusPage.replace("_TXT_STATUS_TEMPERATURE_", FPSTR(txt_status_temperature));
  statusPage.replace("_TXT_STATUS_TELEGRAM_", FPSTR(txt_status_telegram));
  statusPage.replace("_TXT_STATUS_MQTT_", FPSTR(txt_status_mqtt));
  statusPage.replace("_TXT_STATUS_WIFI_", FPSTR(txt_status_wifi));

  if (server.hasArg("mrconn")) mqttConnect();

  String connected = F("<span style='color:#47c266'><b>");
  connected += FPSTR(txt_status_connect);
  connected += F("</b><span>");

  String disconnected = F("<span style='color:#d43535'><b>");
  disconnected += FPSTR(txt_status_disconnect);
  disconnected += F("</b></span>");

  statusPage.replace(F("_CURRENT_TEMPERATURE_"), String(current_temperature));
  statusPage.replace(F("_CURRENT_HUMIDITY_"), String(current_humidity));
  if (temperature_connected) statusPage.replace(F("_TEMPERATURE_STATUS_"), connected);
  else  statusPage.replace(F("_TEMPERATURE_STATUS_"), disconnected);
  if (telegram_connected) statusPage.replace(F("_TELEGRAM_STATUS_"), connected);
  else  statusPage.replace(F("_TELEGRAM_STATUS_"), disconnected);
  if (mqtt_client.connected()) statusPage.replace(F("_MQTT_STATUS_"), connected);
  else statusPage.replace(F("_MQTT_STATUS_"), disconnected);
  statusPage.replace(F("_MQTT_REASON_"), String(mqtt_client.state()));
  statusPage.replace(F("_WIFI_STATUS_"), String(WiFi.RSSI()));
  sendWrappedHTML(statusPage);
}



//login page, also called for logout
void handleLogin() {
  bool loginSuccess = false;
  String msg;
  String loginPage =  FPSTR(html_page_login);
  loginPage.replace("_TXT_LOGIN_TITLE_", FPSTR(txt_login_title));
  loginPage.replace("_TXT_LOGIN_PASSWORD_", FPSTR(txt_login_password));
  loginPage.replace("_TXT_LOGIN_", FPSTR(txt_login));

  if (server.hasHeader("Cookie")) {
    //Found cookie;
    String cookie = server.header("Cookie");
  }
  if (server.hasArg("USERNAME") || server.hasArg("PASSWORD") || server.hasArg("LOGOUT")) {
    if (server.hasArg("LOGOUT")) {
      //logout
      server.sendHeader("Cache-Control", "no-cache");
      server.sendHeader("Set-Cookie", "TEMPERATURESESSIONID=0");
      loginSuccess = false;
    }
    if (server.hasArg("USERNAME") && server.hasArg("PASSWORD")) {
      if (server.arg("USERNAME") == "admin" &&  server.arg("PASSWORD") == login_password) {
        server.sendHeader("Cache-Control", "no-cache");
        server.sendHeader("Set-Cookie", "TEMPERATURESESSIONID=1");
        loginSuccess = true;
        msg = F("<span style='color:#47c266;font-weight:bold;'>");
        msg += FPSTR(txt_login_sucess);
        msg += F("<span>");
        loginPage += F("<script>");
        loginPage += F("setTimeout(function () {");
        loginPage += F("window.location.href= '/';");
        loginPage += F("}, 3000);");
        loginPage += F("</script>");
        //Log in Successful;
      } else {
        msg = F("<span style='color:#d43535;font-weight:bold;'>");
        msg += FPSTR(txt_login_fail);
        msg += F("</span>");
        //Log in Failed;
      }
    }
  }
  else {
    if (is_authenticated() or login_password.length() == 0) {
      server.sendHeader("Location", "/");
      server.sendHeader("Cache-Control", "no-cache");
      //use javascript in the case browser disable redirect
      String redirectPage = F("<html lang=\"en\" class=\"\"><head><meta charset='utf-8'>");
      redirectPage += F("<script>");
      redirectPage += F("setTimeout(function () {");
      redirectPage += F("window.location.href= '/';");
      redirectPage += F("}, 1000);");
      redirectPage += F("</script>");
      redirectPage += F("</body></html>");
      server.send(302, F("text/html"), redirectPage);
      return;
    }
  }
  loginPage.replace(F("_LOGIN_SUCCESS_"), (String) loginSuccess);
  loginPage.replace(F("_LOGIN_MSG_"), msg);
  sendWrappedHTML(loginPage);
}

void handleUpgrade()
{
  uploaderror = 0;
  String upgradePage = FPSTR(html_page_upgrade);
  upgradePage.replace("_TXT_B_UPGRADE_",FPSTR(txt_upgrade));
  upgradePage.replace("_TXT_BACK_",FPSTR(txt_back));
  upgradePage.replace("_TXT_UPGRADE_TITLE_",FPSTR(txt_upgrade_title));
  upgradePage.replace("_TXT_UPGRADE_INFO_",FPSTR(txt_upgrade_info));
  upgradePage.replace("_TXT_UPGRADE_START_",FPSTR(txt_upgrade_start));

  sendWrappedHTML(upgradePage);
}

void handleUploadDone()
{
  //Serial.printl(PSTR("HTTP: Firmware upload done"));
  bool restartflag = false;
  String uploadDonePage = FPSTR(html_page_upload);
  String content = F("<div style='text-align:center;'><b>Upload ");
  if (uploaderror) {
    content += F("<span style='color:#d43535'>failed</span></b><br/><br/>");
    if (uploaderror == 1) {
      content += FPSTR(txt_upload_nofile);
    } else if (uploaderror == 2) {
      content += FPSTR(txt_upload_filetoolarge);
    } else if (uploaderror == 3) {
      content += FPSTR(txt_upload_fileheader);
    } else if (uploaderror == 4) {
      content += FPSTR(txt_upload_flashsize);
    } else if (uploaderror == 5) {
      content += FPSTR(txt_upload_buffer);
    } else if (uploaderror == 6) {
      content += FPSTR(txt_upload_failed);
    } else if (uploaderror == 7) {
      content += FPSTR(txt_upload_aborted);
    } else {
      content += FPSTR(txt_upload_error);
      content += String(uploaderror);
    }
    if (Update.hasError()) {
      content += FPSTR(txt_upload_code);
      content += String(Update.getError());
    }
  } else {
    content += F("<span style='color:#47c266; font-weight: bold;'>");
    content += FPSTR(txt_upload_sucess);
    content += F("</span><br/><br/>");
    content += FPSTR(txt_upload_refresh);
    content += F("<span id='count'>10s</span>...");
    content += FPSTR(count_down_script);
    restartflag = true;
  }
  content += F("</div><br/>");
  uploadDonePage.replace("_UPLOAD_MSG_", content);
  uploadDonePage.replace("_TXT_BACK_", FPSTR(txt_back));
  sendWrappedHTML(uploadDonePage);
  if (restartflag) {
    delay(500);
    ESP.reset();
  }
}

void handleUploadLoop()
{
  // Based on ESP8266HTTPUpdateServer.cpp uses ESP8266WebServer Parsing.cpp and Cores Updater.cpp (Update)
  //char log[200];
  if (uploaderror) {
    Update.end();
    return;
  }
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    if (upload.filename.c_str()[0] == 0)
    {
      uploaderror = 1;
      return;
    }
    //save cpu by disconnect/stop retry mqtt server
    if (mqtt_client.state() == MQTT_CONNECTED) {
      mqtt_client.disconnect();
      lastMqttRetry = millis();
    }
    //snprintf_P(log, sizeof(log), PSTR("Upload: File %s ..."), upload.filename.c_str());
    //Serial.printl(log);
    uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    if (!Update.begin(maxSketchSpace)) {         //start with max available size
      //Update.printError(Serial);
      uploaderror = 2;
      return;
    }
  } else if (!uploaderror && (upload.status == UPLOAD_FILE_WRITE)) {
    if (upload.totalSize == 0)
    {
      if (upload.buf[0] != 0xE9) {
        if (debugging) Serial.println(PSTR("Upload: File magic header does not start with 0xE9"));
        uploaderror = 3;
        return;
      }
      uint32_t bin_flash_size = ESP.magicFlashChipSize((upload.buf[3] & 0xf0) >> 4);
      if (bin_flash_size > ESP.getFlashChipRealSize()) {
        //Serial.printl(PSTR("Upload: File flash size is larger than device flash size"));
        uploaderror = 4;
        return;
      }
      if (ESP.getFlashChipMode() == 3) {
        upload.buf[2] = 3; // DOUT - ESP8285
      } else {
        upload.buf[2] = 2; // DIO - ESP8266
      }
    }
    if (!uploaderror && (Update.write(upload.buf, upload.currentSize) != upload.currentSize)) {
      //Update.printError(Serial);
      uploaderror = 5;
      return;
    }
  } else if (!uploaderror && (upload.status == UPLOAD_FILE_END)) {
    if (Update.end(true)) { // true to set the size to the current progress
      //snprintf_P(log, sizeof(log), PSTR("Upload: Successful %u bytes. Restarting"), upload.totalSize);
      //Serial.printl(log)
    } else {
      //Update.printError(Serial);
      uploaderror = 6;
      return;
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (debugging) Serial.println(PSTR("Upload: Update was aborted"));
    uploaderror = 7;
    Update.end();
  }
  delay(0);
}

void write_log(String message) {
  File logFile = SPIFFS.open(console_file, "a");
  logFile.println(message);
  logFile.close();
}

// MQTT CALLBACK

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  payload[length] = '\0';
  String action = String((char*)payload);

  // ignore heartbeats
  if (strcmp(topic, mqtt_heartbeat_topic.c_str()) == 0) {
    return;
  }
  // ignore state
  if (strcmp(topic, ha_state_topic.c_str()) == 0) {
    return;
  }
 
  if (debugging) {Serial.print("Received MQTT callback ["); Serial.print(topic); Serial.println(F("]"));}
  // if (debugOutput) write_log("<CB-" + action);


  // Receive command
  if (strcmp(topic, ha_command_topic.c_str()) == 0) {
    if (action == "OFF") {
      if (debugging) Serial.println(action); 
      power_status = false;
      digitalWrite(temperature_relay_pin, HIGH);
    } else if (action == "ON") {
      if (debugging) Serial.println(action); 
      power_status = true;
      digitalWrite(temperature_relay_pin, LOW);
    } else {
      if (debugging) Serial.println("action unknown: " + action);       
    }

    // update home assistant state
    mqtt_client.publish(ha_state_topic.c_str(), action.c_str());
        
  } else {
    if (debugging) Serial.println(F("topic unknown")); 
  }

  temperature_connected = true;
}


void powerToggle() {
  if (power_status) {
    powerOff();
  } else {
    powerOn();
  }
}

void powerOn() {
  if (!power_status) {
    mqtt_client.publish(ha_command_topic.c_str(), "ON");
  }
}

void powerOff() {
  if (power_status) {
    mqtt_client.publish(ha_command_topic.c_str(), "OFF");
  }
}


void haConfig() {
  // send HA config packet
  const size_t capacity = JSON_OBJECT_SIZE(10) + 256;
  DynamicJsonDocument haConfig(capacity);

  haConfig["name"] = mqtt_fn;
  haConfig["unique_id"] = getId();

  haConfig["cmd_t"] = ha_command_topic; // command_topic
  haConfig["stat_t"] = ha_state_topic; // state_topic
  haConfig["qos"] = 1; // quality of service
  haConfig["pl_on"] = "ON"; // payload_on
  haConfig["pl_off"] = "OFF"; // payload_off
  haConfig["ret"] = true; // retain

  String mqttOutput;
  serializeJson(haConfig, mqttOutput);
  mqtt_client.beginPublish(ha_config_topic.c_str(), mqttOutput.length(), true);
  mqtt_client.print(mqttOutput);
  mqtt_client.endPublish();
}

void mqttConnect() {
  // Loop until we're reconnected
  int attempts = 0;
  while (!mqtt_client.connected()) {
    // Attempt to connect
    if (debugging) Serial.println(F("Connecting to MQTT..."));
    mqtt_client.connect(mqtt_client_id.c_str(), mqtt_username.c_str(), mqtt_password.c_str());
    // If state < 0 (MQTT_CONNECTED) => network problem we retry 5 times and then waiting for MQTT_RETRY_INTERVAL_MS and retry repeatly
    if (mqtt_client.state() < MQTT_CONNECTED) {
      if (attempts == 5) {
        lastMqttRetry = millis();
        if (debugging) Serial.print("MQTT connection problem");
        write_log("MQTT connection problem");
        if (telegram_connected) {
          bot->sendMessage(telegram_chat_id, program_name + ": can't connect to MQTT ", "");
        }
        return;
      }
      else {
        delay(10);
        attempts++;
      }
    }
    // If state > 0 (MQTT_CONNECTED) => config or server problem we stop retry
    else if (mqtt_client.state() > MQTT_CONNECTED) {
      return;
    }
    // We are connected
    else {
      String topic = mqtt_topic + "/" + mqtt_fn +  "/#";
      
      if (debugging) Serial.println("Connected to MQTT.  Listening for topic: " + topic);
      write_log("Connected to MQTT.  Listening for topic: " + topic);
      if (telegram_connected) {
        bot->sendMessage(telegram_chat_id, program_name + ": connected to MQTT.  Listening for topic: " + topic, "");
      }

      // subscibe
      mqtt_client.subscribe(topic.c_str());

      if (others_haad) {
        haConfig();
      }
    }
  }
}

bool connectWifi() {
  WiFi.hostname(hostname.c_str());
  if (WiFi.getMode() != WIFI_STA) {
    WiFi.mode(WIFI_STA);
    delay(10);
  }
  WiFi.begin(ap_ssid.c_str(), ap_pwd.c_str());
  secured_client.setTrustAnchors(&cert); // Add root certificate for api.telegram.org
  if (debugging) Serial.print("Connecting to " + ap_ssid);
  wifi_timeout = millis() + 30000;
  while (WiFi.status() != WL_CONNECTED && millis() < wifi_timeout) {
    if (debugging) Serial.write('.');
    //if (debugging) Serial.print(WiFi.status());
    // wait 500ms, flashing the blue LED to indicate WiFi connecting...
    digitalWrite(blueLedPin, LOW);
    delay(250);
    digitalWrite(blueLedPin, HIGH);
    delay(250);
  }
  if (debugging) Serial.println(F(""));
  if (WiFi.status() != WL_CONNECTED) {
    if (debugging) Serial.println(F("Failed to connect to wifi"));
    return false;
  }

  while (WiFi.localIP().toString() == "0.0.0.0" || WiFi.localIP().toString() == "") {
    if (debugging) Serial.write('.');
    delay(500);
  }
  if (WiFi.localIP().toString() == "0.0.0.0" || WiFi.localIP().toString() == "") {
    if (debugging) Serial.println(F("Failed to get IP address"));
    return false;
  }
  
  if (debugging) Serial.print(F("WiFi connected. IP address: ")); Serial.println(WiFi.localIP());
  write_log("WiFi connected.");

  //keep LED off (For Wemos D1-Mini)
  digitalWrite(blueLedPin, HIGH);
  return true;
}

String getId() {
  uint32_t chipID = ESP.getChipId();
  return String(chipID, HEX);
}

//Check if header is present and correct
bool is_authenticated() {
  if (server.hasHeader("Cookie")) {
    //Found cookie;
    String cookie = server.header("Cookie");
    if (cookie.indexOf("TEMPERATURESESSIONID=1") != -1) {
      //Authentication Successful
      return true;
    }
  }
  //Authentication Failed
  return false;
}

void checkLogin() {
  if (!is_authenticated() and login_password.length() > 0) {
    server.sendHeader("Location", "/login");
    server.sendHeader("Cache-Control", "no-cache");
    //use javascript in the case browser disable redirect
    String redirectPage = F("<html lang=\"en\" class=\"\"><head><meta charset='utf-8'>");
    redirectPage += F("<script>");
    redirectPage += F("setTimeout(function () {");
    redirectPage += F("window.location.href= '/login';");
    redirectPage += F("}, 1000);");
    redirectPage += F("</script>");
    redirectPage += F("</body></html>");
    server.send(302, F("text/html"), redirectPage);
    return;
  }
}

bool checkBound(float newValue, float prevValue, float maxDiff)
{
  return !isnan(newValue) &&
         (newValue < prevValue - maxDiff || newValue > prevValue + maxDiff);
}

void loop() {
  server.handleClient();
  ArduinoOTA.handle();
  
  //reset board to attempt to connect to wifi again if in ap mode or wifi dropped out and time limit passed
  if (WiFi.getMode() == WIFI_STA and WiFi.status() == WL_CONNECTED) {
    wifi_timeout = millis() + WIFI_RETRY_INTERVAL_MS;
  } else if (wifi_config_exists and millis() > wifi_timeout) {
    ESP.restart();
  }
  
  if (!captive) {
    if (mqtt_config) {
      //MQTT failed retry to connect
      if (mqtt_client.state() < MQTT_CONNECTED)
      {
        if ((millis() > (lastMqttRetry + MQTT_RETRY_INTERVAL_MS)) or lastMqttRetry == 0) {
          mqttConnect();
        }
      }
      //MQTT config problem on MQTT do nothing
      else if (mqtt_client.state() > MQTT_CONNECTED ) return;
      //MQTT connected send status
      else {
        mqtt_client.loop();  

        // send our heartbeat
        if (timeDiff(lastHeartbeatSent, millis()) > HEARTBEAT_SEND_INTERVAL_MS) {
            lastHeartbeatSent = millis();
            if (debugging) Serial.println("Sending heartbeat. On: " + mqtt_heartbeat_topic);
            // if (debugOutput) write_log("HB>");
            mqtt_client.publish(mqtt_heartbeat_topic.c_str(), mqtt_heartbeat.c_str());
        }   
      }
    }

    // only if we have temperature config 
    if (temperature_config) {
      long now = millis();

      // time delay 
      if (now - last_temperature_reading > TEMPERATURE_READING_INTERVAL_MS)
      {
        last_temperature_reading = now;

        // read sensor
        float newTemp = dht.getTemperature();
        float newHum = dht.getHumidity();
        bool changed = false;

        // temperature changed?
        if (checkBound(newTemp, current_temperature, temp_diff))
        {
          current_temperature = newTemp;
          changed = true;
        }

        // humidity changed?
        if (checkBound(newHum, current_humidity, hum_diff))
        {
          current_humidity = newHum;
          changed = true;
        }

        // publish changes 
        if (changed == true) 
        {       
          String payload = "{ \"temperature\": " + String(current_temperature) + ",\"humidity\": " + String(current_humidity) + "}";
          if (debugging) Serial.println(payload);
          if (mqtt_config) {
            mqtt_client.publish(ha_state_topic.c_str(), payload.c_str());
          }

          // relay delay to prevent switching too often
          if (now - last_relay_toggle > RELAY_DELAY_MS) {
            last_relay_toggle = now;

            // limit hit
            if (temperature_limit <= current_temperature) {
              powerOn();
            } else {
              powerOff();
            }
          }

        }
      }
    }
  }
  else {
    dnsServer.processNextRequest();
  }
}
