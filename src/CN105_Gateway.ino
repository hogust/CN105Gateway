//#include <WiFiClient.h>
//#include <CertStoreBearSSL.h>
//#include <WiFiClientSecure.h>
//#include <ESP8266WiFiType.h>
//#include <WiFiServerSecure.h>
//#include <ESP8266WiFiMulti.h>
//#include <ESP8266WiFiGratuitous.h>
//#include <ESP8266WiFiSTA.h>
//#include <ArduinoWiFiServer.h>
//#include <ESP8266WiFiAP.h>
//#include <WiFiServer.h>
//#include <ESP8266WiFiGeneric.h>
//#include <BearSSLHelpers.h>
//#include <WiFiClientSecureBearSSL.h>
//#include <ESP8266WiFiScan.h>
//#include <WiFiServerSecureBearSSL.h>
//#include <ESP8266WiFi.h>
//#include <WiFiUdp.h>






/*
   CN105_Gateway
   Connects Mutsubishi ecodan to MQTT
   decode with help of: 
   https://github.com/m000c400/Mitsubishi-CN105-Protocol-Decode
   part of the code:
   https://github.com/SwiCago/HeatPump

*/
//#include <ESP8266WebServerSecure.h>
#include <ESP8266WebServer.h>
//#include <ESP8266WebServer-impl.h>
//#include <Parsing-impl.h>
//#include <Uri.h>
//#include <WiFi.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <ESP8266HTTPClient.h> //#include <HTTPClient.h>
//#include <WebServer.h>
#include <ArduinoOTA.h>
#include "CN105_Gateway.h"
#include "passwords.h"

#define RXD2 16
#define TXD2 17
#define SERIAL_SIZE_RX 256

WiFiClient espClient;
PubSubClient mqttClient(espClient);
WiFiServer TelnetServer(23);
WiFiClient Telnet;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "nl.pool.ntp.org", 3600, 300000);
ESP8266WebServer webServer(80);

const char* ssid = LOCAL_SSID;                  // put in your ssid
const char* password = SECRET_WIFI_PASSWORD;     // put in your wifi password
const char* mqtt_server = MQTT_SERVER_IP; // put in your ip-address 10.1.1.1
const char* MQTTuser = MQTT_USER;
const char* MQTTpassword = MQTT_PASSWORD;
const char* gatewayName = GATEWAYNAME;    // give your gateway a name

char mqttData[250];                       // receiving mqtt data. Filled by ISR: mqttCallback
char mqttTopic[250];                      // receiving mqtt topic. Filled by ISR: mqttCallback
volatile boolean mqttCmdReceived = false; // flag received mqtt request
boolean flagOTA = true;                   // If OTA active or not
long lastMsg = -600000;     // used to fire the status update and sync the time
long telnetTimer = -1000;   // make sure it fires the first time without waiting
long autoRunTimer = -50000; // used to poll the heat pump
byte commandIndex = 0;      // which item are we polling
long commandTimer;          // polling speed counter
int failedMqttConnect = 0;
boolean connectedToHP = false;

void setup()
{
  //Serial1.begin(115200, SERIAL_8N1);
  //Serial1.println();
  //Serial1.setDebugOutput(true);
  Serial.setRxBufferSize(SERIAL_SIZE_RX);
  Serial.begin(2400, SERIAL_8E1);
  Serial.swap();
  //Serial1.println("Serial init");
  initWifi();
  mqttClient.setServer(mqtt_server, 1883);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(1024);
  timeClient.begin();
  delay(10);
  initWebserver();
  initOTA();
  delay(100);
  TelnetServer.begin();
  TelnetServer.setNoDelay(true);
  delay(100);
} // end setup
void loop()
{
  if (millis() - telnetTimer > 250)
  {
    telnetTimer = millis();
    handleTelnet();
    checkWiFiConnection();
    if (!mqttClient.connected())
    {
      reconnect();
    }
  }
  receiveSerialPacket();
  if (millis() - autoRunTimer > 10000)
  {
    if (!connectedToHP) {
      sendConnectPacket();
      autoRunTimer = millis();
    }
    else {
      autoRunProcess();
    }
  }
  if (mqttCmdReceived)
  {
    processMqttData();
    mqttCmdReceived = false;
  }
  webServer.handleClient(); // WebServer process
  mqttClient.loop();        // MQTT server process
  if (flagOTA)
    ArduinoOTA.handle(); // OTA process
  if (millis() - lastMsg > 600000)
  { // run each xx seconds
    lastMsg = millis();
    publishGatewayStatus();
    timeClient.update();
  }
  delay(1);
} // end loop
// Packet handling routines
void processMqttData()
{
  byte sendBuffer3[packetBufferSize]; // sendbuffer3 is used for send commands received by MQTT
  Telnet.print("Processing MQTT data.  Topic: ");
  Telnet.print(mqttTopic);
  Telnet.print("   Data: ");
  Telnet.println(mqttData);
  StaticJsonDocument<256> doc;
  auto error = deserializeJson(doc, mqttData);
  if (error)
  {
    Telnet.println("parseObject() failed");
    return;
  }
  for (byte item = 0; item < sizeof(commandItems) / sizeof(commandItems[0]); item++)
  {
    const char *command = doc[commandItems[item].VarLongName];
    if (command != nullptr)
    {
      if (encodePacket(sendBuffer3, item, command))
      {
        sendSerialPacket(sendBuffer3);
        Telnet.print("To WP  : ");
        printPacketHex(sendBuffer3);
      }
      else
      {
        Telnet.println("Wrong mqtt data received");
      }
    }
  }
}
void sendSerialPacket(byte *sendBuffer)
{
  int i;
  byte packetLength;
  byte checkSum;

  checkSum = calculateCheckSum(sendBuffer);
  packetLength = sendBuffer[4] + 5;
  sendBuffer[packetLength] = checkSum;
  for (i = 0; i <= packetLength; i++)
  {
    Serial.write(sendBuffer[i]);
  }
  Serial.flush();
}
void sendConnectPacket()
{
  int i;
  for (i = 0; i < CONNECT_LEN; i++)
  {
    Serial.write(CONNECT[i]);
  }
  Serial.flush();
  //connectedToHP = true;
  Telnet.println("Connect packet send");
}
boolean encodePacket(byte *sendBuffer, byte item, const char *command)
{
  memcpy(sendBuffer, commandItems[item].packetMask, packetBufferSize);
  switch (commandItems[item].SetType)
  {
  case SetCommand_SetFlowTemperatureZone1:
    return encodeTemperature(sendBuffer, item, command);
    break;
  case SetCommand_SetRoomTemperatureZone1:
    return encodeTemperature(sendBuffer, item, command);
    break;
  case SetCommand_SetBoilerTemperature:
    return encodeTemperature(sendBuffer, item, command);
    break;
  case SetCommand_SetACModeZone1:
    return encodeACMode(sendBuffer, item, command);
    break;
  case SetCommand_SetOnOff:
    return encodeOnOff(sendBuffer, item, command);
    break;
  case SetCommand_Nothing:
    return true;
    break;
  default:
    return false;
    break;
  }
  return false;
}
boolean encodeACMode(byte *sendBuffer, byte item, const char *command)
{
  int mode;
  mode = command[0] - '0';
  if (mode > 5)
    return false;
  if (mode < 0)
    return false;
  sendBuffer[commandItems[item].VarIndex] = mode;
  return true;
}
boolean encodeTemperature(byte *sendBuffer, byte item, const char *command)
{
  int temperature = atoi(command);
  if (temperature > 70)
    return false;
  if (temperature < 0)
    return false;
  temperature = temperature * 100;
  sendBuffer[commandItems[item].VarIndex] = temperature / 256;
  sendBuffer[commandItems[item].VarIndex + 1] = temperature;
  return true;
}
boolean encodeOnOff(byte *sendBuffer, byte item, const char *command)
{
  int mode;
  mode = command[0] - '0';
  if (mode > 1)
    return false;
  if (mode < 0)
    return false;
  sendBuffer[commandItems[item].VarIndex] = mode;
  return true;
}
void autoRunProcess()
{
  byte sendBuffer[packetBufferSize] = {0xfc, 0x42, 0x02, 0x7a, 0x10, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  if (millis() - commandTimer > 2000)
  {
    commandTimer = millis();
    sendBuffer[5] = commandEntrys[commandIndex];
    sendSerialPacket(sendBuffer);
    Telnet.print("To WP  : ");
    printPacketHex(sendBuffer);
    commandIndex++;
    if (commandIndex == sizeof(commandEntrys))
    {
      commandIndex = 0;
      autoRunTimer = millis();
    }
  }
}
void receiveSerialPacket()
{
  byte receiveBuffer[packetBufferSize];
  char jsonStr[1024];
  char timeStr[20];           // holds the time in a str format

  if (readPacket(receiveBuffer) == RCVD_PKT_CONNECT_SUCCESS)
  {
    timeClient.getFormattedTime().toCharArray(timeStr, 9);
    Telnet.println(timeStr);
    Telnet.print("From WP: ");
    printPacketHex(receiveBuffer);
    parsePacket(receiveBuffer, jsonStr);
    char gatewayTeleTopic[100] = {'\0'};
    strcat(gatewayTeleTopic, gatewayName);
    strcat(gatewayTeleTopic, "/tele/");
    char tempStr[10];
    sprintf(tempStr, "0x%02X", receiveBuffer[5]);
    strcat(gatewayTeleTopic, tempStr);
    Telnet.print("Sending MQTT messages: ");
    Telnet.print(gatewayTeleTopic);
    Telnet.print(" ");
    Telnet.println(jsonStr);
    mqttClient.publish(gatewayTeleTopic, jsonStr);
  }
}
int readPacket(byte *data)
{
  bool foundStart = false;
  int dataSum = 0;
  byte checksum = 0;
  byte dataLength = 0;
  if (Serial.available() > 0)
  { // read until we get start byte 0xfc
    while (Serial.available() > 0 && !foundStart)
    {
      data[0] = Serial.read();
      if (data[0] == HEADER[0])
      {
        foundStart = true;
        delay(100); // found that this delay increases accuracy when reading, might not be needed though
      }
    }
    if (!foundStart)
    {
      return RCVD_PKT_FAIL;
    }
    for (int i = 1; i < 5; i++) //read header
    {
      data[i] = Serial.read();
    }
    if (data[0] == HEADER[0]) //check header
    {
      dataLength = data[4] + 5;
      for (int i = 5; i < dataLength; i++)
      {
        data[i] = Serial.read(); // read the payload data
      }
      data[dataLength] = Serial.read();   // read checksum byte
      for (int i = 0; i < dataLength; i++) // sum up the header bytes...
      {
        dataSum += data[i];
      }
      checksum = (0xfc - dataSum) & 0xff; // calculate checksum
      if (data[dataLength] == checksum)
      { // we have a correct packet
        if (data[1] == 0x7a){
          connectedToHP = true;
          Telnet.println("Connected successfully");
        }
        return RCVD_PKT_CONNECT_SUCCESS;
      }
      else
      {
        Telnet.println("CRC ERROR");
      }
    }
  }
  return RCVD_PKT_FAIL;
}
void printPacketHex(byte *receivePacket)
{
  char receivePacketStr[packetBufferSize * 5];
  Bin2Hex(receivePacketStr, receivePacket);
  Telnet.println(receivePacketStr);
}
byte calculateCheckSum(byte *data)
{
  byte dataLength = 0;
  int dataSum = 0;
  byte checksum = 0;

  dataLength = data[4] + 5;
  for (int i = 0; i < dataLength; i++) // sum up the header bytes...
  {
    dataSum += data[i];
  }
  checksum = (0xfc - dataSum) & 0xff; // calculate checksum
  return checksum;
}
void Bin2Hex(char *hexStr, byte *packet)
{
  byte packetLen;

  packetLen = packet[4] + 5;
  int i;
  for (i = 0; i < packetLen; i++)
  {
    sprintf(hexStr + i * 5, "0x%02X ", packet[i]);
  }
  hexStr[packetLen * 5 + 1] = '\0';
}
// packet parsing routines
void parsePacket(byte *packet, char *jsonStr)
{
  int i = 0;
  // Bin2Hex(textStr, packet); // we have the char hex value in the str. Now we replace the know fields
  jsonStr[0] = '{';
  jsonStr[1] = '\0';
  i = 0;
  while (items[i].PacketType != 0)
  {
    if (packet[1] == items[i].PacketType)
    {
      if (packet[5] == items[i].Command)
      {
        parsePacketItem(packet, jsonStr, i);
      }
    }
    i++;
  }
  strcat(jsonStr, "}");
}
void parsePacketItem(byte *packet, char *jsonStr, byte item)
{
  char tmpStr[50];
  switch (items[item].VarType)
  {
  case VarType_TIME_DATE:
    parseTimeDate(packet, items[item].VarIndex, tmpStr);
    break;
  case VarType_TEMPERATURE:
    parseTemperature(packet, items[item].VarIndex, tmpStr);
    break;
  case VarType_ONE_BYTE_TEMPERATURE:
    parseOneByteTemperature(packet, items[item].VarIndex, tmpStr);
    break;
  case VarType_POWER_STATE:
    parsePowerState(packet, items[item].VarIndex, tmpStr);
    break;
  case VarType_OPERATION_MODE:
    parseOperationMode(packet, items[item].VarIndex, tmpStr);
    break;
  case VarType_WATERFLOW:
    break;
  case VarType_HW_MODE:
    parseHotWaterMode(packet, items[item].VarIndex, tmpStr);
    break;
  case VarType_MODE_SETTING:
    parseModeSetting(packet, items[item].VarIndex, tmpStr);
    break;
  case VarType_DEFROST:
    parseDeFrost(packet, items[item].VarIndex, tmpStr);
    break;
  case VarType_HEAT_COOL:
    parseHeatCool(packet, items[item].VarIndex, tmpStr);
    break;
  case VarType_HEXVALUE:
    parseHexValue(packet, items[item].VarIndex, tmpStr);
    break;
  case VarType_DECVALUE:
    parseDecValue(packet, items[item].VarIndex, tmpStr);
    break;
  case VarType_RUNTIME:
    parseRuntime(packet, items[item].VarIndex, tmpStr);
    break;
  case VarType_ONE_BYTE_TEMPERATURE_20:
    parseOneByteTemperature20(packet, items[item].VarIndex, tmpStr);
    break;
  case VarType_DATE:
    parseDate(packet, items[item].VarIndex, tmpStr);
    break;
  case VarType_3BYTEVALUE:
    parse3ByteValue(packet, items[item].VarIndex, tmpStr);
    break;
  case VarType_2BYTEHEXVALUE:
    parse2ByteHexValue(packet, items[item].VarIndex, tmpStr);
    break;
  case VarType_OnOff:
    parseOnOff(packet, items[item].VarIndex, tmpStr);
    break;
  default:
    break;
  }
  //memcpy(textStr + items[item].VarIndex * 5, items[item].VarShortName, 4);
  if (strlen(jsonStr) > 2)
  {
    strcat(jsonStr, ",");
  }
  strcat(jsonStr, "\"");
  strcat(jsonStr, items[item].noSpacesName);
  strcat(jsonStr, "\":\"");
  strcat(jsonStr, tmpStr);
  strcat(jsonStr, "\"");
}
void parseTemperature(byte *packet, byte varIndex, char *textStr)
{
  float temperature;
  float msb = 0;
  float lsb = 0;
  msb = packet[varIndex];
  lsb = packet[varIndex + 1];
  temperature = (msb * 256 + lsb) / 100;
  sprintf(textStr, "%2.1f", temperature);
}
void parseTimeDate(byte *packet, byte varIndex, char *textStr)
{
  sprintf(textStr, "20%d/%02d/%02d %02d:%02d:%02d", packet[varIndex],
          packet[varIndex + 1], packet[varIndex + 2], packet[varIndex + 3], packet[varIndex + 4], packet[varIndex + 5]);
}
void parseOneByteTemperature(byte *packet, byte varIndex, char *textStr)
{
  float temperature;
  float msb = 0;
  msb = packet[varIndex];
  temperature = msb / 2 - 39;
  sprintf(textStr, "%2.1f", temperature);
}
void parsePowerState(byte *packet, byte varIndex, char *textStr)
{
  switch (packet[varIndex])
  {
  case 0:
    strcpy(textStr, "Standby");
    break;
  case 1:
    strcpy(textStr, "On");
    break;
  default:
    sprintf(textStr, "Unknown entry: %02X", packet[varIndex]);
    break;
  }
}
void parseModeSetting(byte *packet, byte varIndex, char *textStr)
{
  switch (packet[varIndex])
  {
  case 0:
    strcpy(textStr, "Heating Room Temp");
    break;
  case 1:
    strcpy(textStr, "Heating Flow Temp");
    break;
  case 2:
    strcpy(textStr, "Heating Heat Curve");
    break;
  case 3:
    strcpy(textStr, "Cooling Room Temp");
    break;
  case 4:
    strcpy(textStr, "Cooling Flow Temp");
    break;
  case 5:
    strcpy(textStr, "Floor Dryup");
    break;
  default:
    sprintf(textStr, "Unknown: 0x%02X", packet[varIndex]);
    break;
  }
}
void parseHotWaterMode(byte *packet, byte varIndex, char *textStr)
{
  switch (packet[varIndex])
  {
  case 0:
    strcpy(textStr, "Normal");
    break;
  case 1:
    strcpy(textStr, "Economy");
    break;
  default:
    sprintf(textStr, "Unknown entry: 0x%02X", packet[varIndex]);
    break;
  }
}
void parseDeFrost(byte *packet, byte varIndex, char *textStr)
{
  switch (packet[varIndex])
  {
  case 0:
    strcpy(textStr, "Normal");
    break;
  case 1:
    strcpy(textStr, "Standby");
    break;
  case 2:
    strcpy(textStr, "Defrost");
    break;
  case 3:
    strcpy(textStr, "Waiting Restart");
    break;
  default:
    sprintf(textStr, "Unknown entry: 0x%02X", packet[varIndex]);
    break;
  }
}
void parseHeatCool(byte *packet, byte varIndex, char *textStr)
{
  switch (packet[varIndex])
  {
  case 1:
    strcpy(textStr, "Heating Mode");
    break;
  case 4:
    strcpy(textStr, "Cooling Mode");
    break;
  default:
    sprintf(textStr, "Unknown entry: 0x%02X", packet[varIndex]);
    break;
  }
}
void parseHexValue(byte *packet, byte varIndex, char *textStr)
{
  sprintf(textStr, "0x%02X", packet[varIndex]);
}
void parseOperationMode(byte *packet, byte varIndex, char *textStr)
{
  switch (packet[varIndex])
  {
  case 0:
    strcpy(textStr, "Stop");
    break;
  case 1:
    strcpy(textStr, "Hot Water");
    break;
  case 2:
    strcpy(textStr, "Heating");
    break;
  case 3:
    strcpy(textStr, "Cooling");
    break;
  case 4:
    strcpy(textStr, "No voltage contact input (HW)");
    break;
  case 5:
    strcpy(textStr, "Freeze Stat");
    break;
  case 6:
    strcpy(textStr, "Legionella");
    break;
  case 7:
    strcpy(textStr, "Heating Eco");
    break;
  case 8:
    strcpy(textStr, "Mode 1");
    break;
  default:
    sprintf(textStr, "Unknown: 0x%02X", packet[varIndex]);
    break;
  }
}
void parseDecValue(byte *packet, byte varIndex, char *textStr)
{
  sprintf(textStr, "%d", packet[varIndex]);
}
void parseRuntime(byte *packet, byte varIndex, char *textStr)
{
  int32_t runtime;
  runtime = (packet[varIndex + 1] * 256 + packet[varIndex + 2]) * 100 + packet[varIndex];
  sprintf(textStr, "%d", runtime);
}
void parseOneByteTemperature20(byte *packet, byte varIndex, char *textStr)
{
  float temperature;
  float msb = 0;
  msb = packet[varIndex];
  temperature = msb / 2 - 20;
  sprintf(textStr, "%2.1f", temperature);
}
void parse3ByteValue(byte *packet, byte varIndex, char *textStr)
{
  float value;
  value = (packet[varIndex] * 256 + packet[varIndex + 1]) * 100 + packet[varIndex + 2];
  value = value / 100;
  sprintf(textStr, "%3.2f", value);
}
void parseDate(byte *packet, byte varIndex, char *textStr)
{
  sprintf(textStr, "20%d/%02d/%02d", packet[varIndex],
          packet[varIndex + 1], packet[varIndex + 2]);
}
void parse2ByteHexValue(byte *packet, byte varIndex, char *textStr)
{
  int value;
  value = (packet[varIndex] * 256 + packet[varIndex + 1]);
  sprintf(textStr, "0x%04X", value);
}
void parseOnOff(byte *packet, byte varIndex, char *textStr)
{
  switch (packet[varIndex])
  {
  case 0:
    strcpy(textStr, "Off");
    break;
  case 1:
    strcpy(textStr, "On");
    break;
  default:
    sprintf(textStr, "Unknown entry: %02X", packet[varIndex]);
    break;
  }
}

// system subs
void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  if (mqttCmdReceived)
  {
    Serial.swap();
    Serial.println("Previous MQTT command not processed yet. Skipping this one");
    Serial.swap();
  }
  else
  {
    strcpy(mqttTopic, topic);
    strncpy(mqttData, (char *)payload, length);
    mqttData[length] = '\0';
    Serial.swap();
    Serial.println("MQTT command received");
    Serial.swap();
  }
  mqttCmdReceived = true; // data is in mqttData[] and topic is in mqttTopic[]
}
void reconnect()
{
  // Loop until we're reconnected
  while (!mqttClient.connected())
  {
    Serial.swap();
    Serial.print("Attempting MQTT connection...");
    Serial.swap();
    // Attempt to connect
    char gatewayStatusTopic[100] = {'\0'};
    strcat(gatewayStatusTopic, gatewayName);
    strcat(gatewayStatusTopic, "/status");
    if (mqttClient.connect(gatewayName, MQTTuser, MQTTpassword, gatewayStatusTopic, MQTTQOS0, true, "Connection Lost"))
    {
      Serial.swap();
      Serial.println("connected");
      Serial.swap();
      // Once connected, publish an announcement...
      // ... and resubscribe
      char gatewaySubscribeTopic[100] = {'\0'};
      strcat(gatewaySubscribeTopic, gatewayName);
      strcat(gatewaySubscribeTopic, "/cmnd");
      mqttClient.subscribe(gatewaySubscribeTopic);
    }
    else
    {
      Serial.swap();
      Serial.print("failed to connect to MQTT, rc=");
      Serial.print(mqttClient.state());
      Serial.swap();
      ++failedMqttConnect;
      if (failedMqttConnect > 500)
      {
        Serial.swap();
        Serial.println(" Just reboot we lost the mqtt connection so badly");
        Serial.swap();
        delay(1000);
        ESP.restart();
      }
      delay(1000);
    }
  }
  if (failedMqttConnect > 400)
    failedMqttConnect = 0;
}
void checkWiFiConnection()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    // wifi down, reconnect here
    WiFi.begin();
    Serial.swap();
    Serial.println("WiFi disconnected, will try to reconnect");
    Serial.swap();
    int WLcount = 0;
    int UpCount = 0;
    while (WiFi.status() != WL_CONNECTED && WLcount < 200)
    {
      delay(100);
      Serial.swap();
      Serial.printf(".");
      Serial.swap();
      if (UpCount >= 60) // just keep terminal from scrolling sideways
      {
        UpCount = 0;
        Serial.swap();
        Serial.printf("\n");
        Serial.swap();
      }
      ++UpCount;
      ++WLcount;
    }
    reconnect();
  }
}
void publishGatewayStatus()
{
  // publish gateway status
  //Serial1.println("Waiting for ESP-NOW messages or MQTT commands... 600 sec");
  // publish online
  char gatewayStatusTopic[100] = {'\0'};
  strcat(gatewayStatusTopic, gatewayName);
  strcat(gatewayStatusTopic, "/status");
  //  Serial1.print("MQTT publish: [");
  //  Serial1.print(gatewayStatusTopic);
  //  Serial1.println("] Online");
  mqttClient.publish(gatewayStatusTopic, "Online");
  // public ip address as hyperlink
  char gatewayIPTopic[100] = {'\0'};
  strcat(gatewayIPTopic, gatewayName);
  strcat(gatewayIPTopic, "/ipaddress");
  char msg[100] = {'\0'};
  strcat(msg, "<a href=\"http://");
  char IP[] = "xxx.xxx.xxx.xxx"; // buffer
  IPAddress ip = WiFi.localIP();
  ip.toString().toCharArray(IP, 16);
  strcat(msg, IP);
  strcat(msg, "\"target=\"_blank\">");
  strcat(msg, IP);
  strcat(msg, "</a>");
  // &hs.setdevicestring(1380,"<a href=""http://192.168.5.161""target=""_blank"">Online</a>",true)
  //  Serial1.print("MQTT publish: [");
  //  Serial1.print(gatewayIPTopic);
  //  Serial1.print("] ");
  //  Serial1.println(msg);
  mqttClient.publish(gatewayIPTopic, msg);
} //end publishGatewayStatus
void initWifi()
{
  WiFi.setHostname(gatewayName);
  WiFi.mode(WIFI_STA);
  Serial.swap();
  Serial.print("Connecting to ");
  Serial.print(ssid);
  Serial.swap();
  if (strcmp(WiFi.SSID().c_str(), ssid) != 0)
  {
    WiFi.begin(ssid, password);
  }
  int retries = 20; // 10 seconds
  while ((WiFi.status() != WL_CONNECTED) && (retries-- > 0))
  {
    delay(500);
    Serial.swap();
    Serial.print(".");
  }
  Serial.swap();
  Serial.println("");
  Serial.swap();
  if (retries < 1)
  {
    Serial.swap();
    Serial.print("*** WiFi connection failed");
    Serial.swap();
    ESP.restart();
  }
  Serial.swap();
  Serial.print("WiFi connected, IP address: ");
  Serial.println(WiFi.localIP());
  Serial.print("STA mac: ");
  Serial.println(WiFi.macAddress());
  Serial.print("WiFi channel: ");
  Serial.println(WiFi.channel());
  Serial.println("Hostname " + String(gatewayName));
  Serial.swap();

} // end initWifi
void initOTA()
{
  ArduinoOTA.setHostname(gatewayName);
  ArduinoOTA.onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH)
          type = "sketch";
        else // U_SPIFFS
          type = "filesystem";

        // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
        Serial.swap();
        Serial.println("Start updating " + type);
        Serial.swap();
      });
  ArduinoOTA.onEnd([]() {
        Serial.swap();
        Serial.println("\nEnd");
        Serial.swap();
      });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.swap();
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
        Serial.swap();
      });
  ArduinoOTA.onError([](ota_error_t error) {
        Serial.swap();
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR)
          Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR)
          Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR)
          Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR)
          Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR)
          Serial.println("End Failed");
        Serial.swap();
      });

  ArduinoOTA.begin();
}
void handleTelnet()
{
  if (TelnetServer.hasClient())
  {
    if (!Telnet || !Telnet.connected())
    {
      if (Telnet)
        Telnet.stop();
      Telnet = TelnetServer.available();
    }
    else
    {
      TelnetServer.available().stop();
    }
  }
} // end handleTelnet

// webserver
void initWebserver()
{
  webServer.on("/", handleRoot);
  webServer.on("/restart", handleRestart);
  webServer.on("/updateOTA", handleUpdateOTA);
  webServer.on("/cancelOTA", handleCancelUpdateOTA);
  webServer.onNotFound(handleNotFound);
  webServer.begin();
  Serial.swap();
  Serial.println("HTTP server started");
  Serial.swap();
  delay(10);
}
void handleUpdateOTA()
{
  webServer.send(200, "text/plain", "Going into OTA programming mode");
  flagOTA = true;
}
void handleCancelUpdateOTA()
{
  webServer.send(200, "text/plain", "Canceling OTA programming mode");
  flagOTA = false;
}
void handleRoot()
{
  String message = "Hello from ";
  message += gatewayName;
  message += "\n\nProgram: CN105_Gateway_V2.0\n";
  message += "Telnet to this device for logging output\n";
  message += "\nCurrent time: ";
  message += timeClient.getFormattedTime();
  message += "\nUsages:\n";
  message += "/                  - This messages\n";
  message += "/updateOTA         - Put device in OTA programming mode\n";
  message += "/cancelOTA         - Cancel OTA programming mode\n";
  message += "/restart           - Restarts the CN105 gateway\n\n";
  message += "Programming mode: ";
  message += flagOTA;
  message += "\nGateway Macadres: ";
  message += WiFi.macAddress();
  message += "\nWiFi channel: ";
  message += WiFi.channel();
  message += "\nRSSI: ";
  message += WiFi.RSSI();
  message += "\nOTA flag: ";
  message += flagOTA;
  message += "\nFree memory: ";
  message += ESP.getFreeHeap();
  message += "\nfailedMqttReconnect: ";
  message += failedMqttConnect;
  message += "\n\n";
  webServer.send(200, "text/plain", message);
}
void handleRestart()
{
  webServer.send(200, "text/plain", "OK restarting");
  delay(2000);
  ESP.restart();
}
void handleNotFound()
{
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += webServer.uri();
  message += "\nMethod: ";
  message += (webServer.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += webServer.args();
  message += "\n";
  for (uint8_t i = 0; i < webServer.args(); i++)
  {
    message += " " + webServer.argName(i) + ": " + webServer.arg(i) + "\n";
  }
  webServer.send(404, "text/plain", message);
}
