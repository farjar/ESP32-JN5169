#if CONFIG_FREERTOS_UNICORE
#define ARDUINO_RUNNING_CORE 0
#else
#define ARDUINO_RUNNING_CORE 1
#endif
#include <FS.h>
#include "time.h"
#include "SPIFFS.h"
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include "HardwareSerial.h"
HardwareSerial jnSerial(2);

//OLED Section
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 64 // OLED display height, in pixels
// Declaration for an SSD1306 display connected to I2C (SDA, SCL pins)
#define OLED_RESET     -1 // Reset pin # (or -1 if sharing Arduino reset pin)
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

//Web server
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
AsyncWebSocketClient * globalClient = NULL;
const char* PARAM_MESSAGE = "message";
//---------------------------------
//NTP
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7200;
const int   daylightOffset_sec = 3600;
char dateStringBuff[50]; //50 chars should be enough
char timeStringBuff[50]; //50 chars should be enough

//---------------------------------------
String print_string = "";
String attr_response = "";
// ------task get full info -----
bool new_device_connected = false;
uint16_t new_device_ShortAddr = 0;
uint64_t new_device_LongAddr  = 0;
String NewDevName = "";
int counter = 0;
bool EpResponse = false;
bool ClResponse = false;
bool DnResponse = false;
byte rxMessageData_newDevice[100];
byte ClDataNewDevice[100];
String NewDevComplete = "";
//---------------------------------------
uint64_t au64ExtAddr[16];
byte rxMessageData[1024];
byte rxMessageChecksum = 0;
uint16_t rxMessageLength = 0;
uint8_t rxMessageState    = 0;
uint16_t rxMessageType = 0;
uint8_t rxMessageCount    = 0;
bool rxMessageInEscape = false;
byte rxByte;


#define RXD2 16
#define TXD2 17

void serialEvent() {
  while (jnSerial.available())
  {
    byte rxByte = (byte)jnSerial.read();

    if (rxByte == 0x01)
    {
      // Start character received
      rxMessageChecksum = 0;
      rxMessageLength   = 0;
      rxMessageType     = 0;
      rxMessageState    = 0;
      rxMessageCount    = 0;
      rxMessageInEscape = false;
    }
    else if (rxByte == 0x02)
    {
      rxMessageInEscape = true;
    }
    else if (rxByte == 0x03)
    {
      displayDecodedCommand(rxMessageType, rxMessageLength, rxMessageData);
    }
    else
    {
      if (rxMessageInEscape == true)
      {
        rxByte ^= 0x10;
        rxMessageInEscape = false;
      }

      // Parse character
      switch (rxMessageState)
      {
        case 0:
          {
            rxMessageType = rxByte;
            rxMessageType <<= 8;
            rxMessageState++;
          }
          break;

        case 1:
          {
            rxMessageType |= rxByte;
            rxMessageState++;
          }
          break;

        case 2:
          {
            rxMessageLength = rxByte;
            rxMessageLength <<= 8;
            rxMessageState++;
          }
          break;

        case 3:
          {
            rxMessageLength |= rxByte;
            rxMessageState++;
          }
          break;

        case 4:
          {
            rxMessageChecksum = rxByte;
            rxMessageState++;
          }
          break;

        default:
          {
            rxMessageData[rxMessageCount++] = rxByte;
          }
          break;
      }
    }
  }
}

void TaskDecode( void *pvParameters );
void TaskGetFullInfo( void *pvParameters );

//Web server serup
void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len) {

  if (type == WS_EVT_CONNECT) {

    Serial.println("Websocket client connection received");
    globalClient = client;

  } else if (type == WS_EVT_DISCONNECT) {

    Serial.println("Websocket client connection finished");
    globalClient = NULL;

  }
  else if (type == WS_EVT_DATA) {

    Serial.println("Data received: ");

    for (int i = 0; i < len; i++) {
      Serial.print(data[i]);
      Serial.print("|");
    }

    Serial.println();
  }
}


//gets called when WiFiManager enters configuration mode
void configModeCallback (WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());
  //if you used auto generated SSID, print it
  Serial.println(myWiFiManager->getConfigPortalSSID());
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("Zigbee Gateway v0.1"));
  display.setCursor(0, 10);
  display.println(F("https://nrf52840.ru"));
  display.setCursor(0, 20);
  display.println(F("Wifi config mode"));
  display.setCursor(10, 30);
  display.println(F("Connect to AP"));
  display.setCursor(20, 40);
  display.println(F("ZigBeeGW"));
  display.setCursor(20, 50);
  display.println(F("and Setup WiFi"));
  display.display();
}

void UpdateLocalTime()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    return;
  }
  strftime(dateStringBuff, sizeof(dateStringBuff), "%A, %B %d %Y", &timeinfo);
  strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M:%S", &timeinfo);
  //Serial.println(dateStringBuff);
  //Serial.println(timeStringBuff);

}

void setup() {
  Serial.begin(115200);
  Serial.println("Start");

  jnSerial.begin(115200, SERIAL_8N1, RXD2, TXD2);
  //SPIFFS
  if (!SPIFFS.begin()) {
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  //OLED
  // SSD1306_SWITCHCAPVCC = generate display voltage from 3.3V internally
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { // Address 0x3D for 128x64
    Serial.println(F("SSD1306 allocation failed"));
    for (;;); // Don't proceed, loop forever
  }
  // Wifi Section
  WiFiManager wm;
  //wm.resetSettings();

  wm.setAPCallback(configModeCallback);
  // id/name, placeholder/prompt, default, length
  if (!wm.autoConnect("ZigBeeGW")) {
    Serial.println("failed to connect and hit timeout");
    //reset and try again, or maybe put it to deep sleep
    ESP.restart();
    delay(1000);
  }
  //init and get the time
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  UpdateLocalTime();

  //Oled Show WIFI
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("Zigbee Gateway v0.1"));
  display.setCursor(0, 15);
  display.println(F("https://nrf52840.ru"));
  display.setCursor(0, 30);
  display.println(F("WIFI Connected"));
  display.setCursor(0, 45);
  display.println(F("IP:"));
  display.setCursor(20, 45);
  display.println(WiFi.localIP());
  display.display();

  //Web Server setup
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(SPIFFS, "/ws.html", "text/html");
  });

  server.begin();
  delay(1000);

  ////
  xTaskCreatePinnedToCore(
    TaskDecode
    ,  "TaskDecodeUart"
    ,  16384
    ,  NULL
    ,  2
    ,  NULL
    ,  ARDUINO_RUNNING_CORE);

  xTaskCreatePinnedToCore(
    TaskGetFullInfo
    ,  "GetFullInfoFromConnectedDevice"
    ,  32768
    ,  NULL
    ,  1
    ,  NULL
    ,  ARDUINO_RUNNING_CORE);

  //Hard RESET
  //transmitCommand(0x0012, 0, 0);
  //delay(2000);
  //SOFT RESET
  //transmitCommand(0x0011, 0, 0);
  //Check version of firmware on JN5169
  transmitCommand(0x0010, 0, 0);
  delay(50);
  //Set device type Coordinator
  setDeviceType(0);
  delay(50);
  //Set ZigBee Channel
  setChannel(11);
  delay(50);
  transmitCommand(0x0024, 0, 0);
  delay(50);
  //transmitCommand(0x0015, 0, 0);
  //network state
  transmitCommand(0x0009, 0, 0);
  delay(50);
  DiscoverDevices();
  delay(50);
  sendMgmtLqiRequest(0x0617, 0);
  delay(50);
  setPermitJoin(0x0000, 0xFE, 0x00);
  delay(50);
  transmitCommand(0x0014, 0, 0);
  delay(50);
  transmitCommand(0x0017, 0, 0);
  delay(50);
  //sendReadAttribRequest(0x5465, 1, 1 , 0 , 0, 0, 0, 1, 0x0005);
  //void sendReadAttribRequest(uint16_t u16ShortAddr, byte u8SrcEndPoint, byte u8DstEndPoint, uint16_t u16ClusterID, byte u8Direction, byte u8ManuSpecific, uint16_t u16ManuID, byte u8AttribCount, uint16_t u16AttribID1)
}

void ShowOled()
{
  UpdateLocalTime();
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("Zigbee Gateway v0.1"));
  display.setCursor(0, 15);
  display.println(F("IP:"));
  display.setCursor(20, 15);
  display.println(WiFi.localIP());
  display.setCursor(0, 30);
  display.println(dateStringBuff);
  display.setCursor(0, 50);
  display.println(timeStringBuff);
  display.display();
}

void loop() {
  //Serial.printf("Internal Total heap %d, internal Free Heap %d\n", ESP.getHeapSize(), ESP.getFreeHeap());
  ShowOled();
  delay(1000);
  //sendClusterOnOff(2,0x5465,1,1,2);
}

/*--------------------------------------------------*/
/*---------------------- Tasks ---------------------*/
/*--------------------------------------------------*/

void TaskDecode(void *pvParameters)  // This is a task.
{
  (void) pvParameters;

  for (;;) // A Task shall never return or exit.
  {
    serialEvent();
    vTaskDelay(10);  // one tick delay (15ms) in between reads for stability
  }
}


void TaskGetFullInfo(void *pvParameters)  // This is a task.
{
  (void) pvParameters;

  for (;;) // A Task shall never return or exit.
  {
    if (new_device_connected) {
      new_device_connected = false;
      NewDevComplete = "";
      activeEndpointDescriptorRequest(new_device_ShortAddr);
      counter = 500;
      while (!EpResponse) {
        delay(1);
        if (counter-- == 0) {
          break;
        }
      }
      sendReadAttribRequest(new_device_ShortAddr, 1, rxMessageData_newDevice[1] , 0 , 0, 0, 0, 1, 0x0005);
      counter = 500;
      while (!DnResponse) {
        delay(1);
        if (counter-- == 0) {
          break;
        }
      }
      NewDevComplete += "{ ";
      NewDevComplete += NewDevName + " ; ";
      NewDevComplete += "0x" + String(new_device_ShortAddr, HEX) + " ; ";
      for (int i = 0; i < rxMessageData_newDevice[0]; i++)
      {
        NewDevComplete += "Ep";
        NewDevComplete += String(i, DEC) + ":";
        NewDevComplete += String(rxMessageData_newDevice[i + 1], HEX) + " ; ";
        simpleDescriptorRequest(new_device_ShortAddr, rxMessageData_newDevice[i + 1]);
        counter = 500;
        while (!ClResponse) {
          delay(1);
          if (counter-- == 0) {
            break;
          }
        }
        ///Get clusters
        byte u8Length = 0;
        u8Length = ClDataNewDevice[0];
        if (u8Length > 0)
        {
          byte u8InputClusterCount = 0;
          u8InputClusterCount = ClDataNewDevice[7];
          NewDevComplete += "Clusters ";
          for (int i = 0; i < u8InputClusterCount; i++)
          {
            uint16_t u16ClusterId = 0;
            u16ClusterId = ClDataNewDevice[(i * 2) + 8];
            u16ClusterId <<= 8;
            u16ClusterId |= ClDataNewDevice[(i * 2) + 9];
            NewDevComplete += ": 0x" + String(u16ClusterId, HEX);
          }
        }
      }
      NewDevComplete += " }";
      Serial.println(NewDevComplete);
      EpResponse = false; DnResponse = false; ClResponse = false;
      memset(rxMessageData_newDevice, 0, sizeof(rxMessageData_newDevice));
      memset(ClDataNewDevice, 0, sizeof(ClDataNewDevice));
    }
    vTaskDelay(10);  // one tick delay (15ms) in between reads for stability
  }
}
