#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <SoftwareSerial.h>


/************ NEXTION SETTINGS ******************/
byte nextionReturnBuffer[128];                      // Byte array to pass around data coming from the panel
uint8_t nextionReturnIndex = 0;                     // Index for nextionReturnBuffer
bool lcdConnected = false;                          // Set to true when we've heard something from the LCD
bool debugSerialEnabled = true;                     // Enable USB serial debug output
const byte nextionSuffix[] = {0xFF, 0xFF, 0xFF};    // Standard suffix for Nextion commands

/************ WIFI and MQTT INFORMATION (CHANGE THESE FOR YOUR SETUP) ******************/
#define wifi_ssid "**your_wifi_ssid**" //type your WIFI information inside the quotes
#define wifi_password "**your_wifi_password**"
#define mqtt_server "**mqtt_server_ip**"
#define mqtt_user "**mqtt_username**"
#define mqtt_password "**mqtt_password**"
#define mqtt_port 1883 //mqtt port

/**************************** FOR OTA **************************************************/
#define SENSORNAME "hasp-entrance"
#define OTApassword "**your_ota_password**" // change this to whatever password you want to use when you upload OTA
int OTAport = 8266;

/************* MQTT TOPICS (change these topics as you wish)  **************************/
#define panel_state_topic "hasp/entrance"
#define panel_command_topic "hasp/entrance/set"
#define alarm_state_topic "home/alarm"
#define alarm_command_topic "home/alarm/set"

#define BUZZERPIN    D6

const char* on_cmd = "on";
const char* off_cmd = "off";

bool stateOn = false; //if screen is on and working
bool lightsOn = true; //if any house light is on
String alarmStatus = "disarmed";
String alarmStatus_old = "disarmed";
String insideTemp = "0";
String outsideTemp = "0";
String rainfall = "0";
String activePage = "home";
int press_count = 0;
String masterCode = "1234";
int alarmCode[] = { 0, 0, 0, 0 };
int codeWidth = 0;
int codeWidthWipe = 147;
unsigned long pendingElapsed = 0;
unsigned long transitionTimer = 0;
bool longPress = false; //if we have held the button down in the correct area of the screen
unsigned long pressTimer = 0;
unsigned long dimScreenDuration = 60; //duration before we dim the screen in seconds
String dimmedValue = "10"; //dim level from 0 - 100
bool dimmed = false; //records whether we have already dimmed the screen

const int BUFFER_SIZE = 300;
#define MQTT_MAX_PACKET_SIZE 512

WiFiClient espClient;
PubSubClient client(espClient);

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);  // Serial - LCD RX (after swap), debug TX
  Serial1.begin(115200); // Serial1 - LCD TX, no RX
  Serial.swap();

  while (!lcdConnected && (millis() < 5000))
  { // Wait up to 5 seconds for serial input from LCD
    nextionHandleInput();
  }
  if (lcdConnected)
  {
    debugPrintln(F("HMI: LCD responding, continuing program load"));
    nextionSendCmd("connect");
  }
  else
  {
    debugPrintln(F("HMI: LCD not responding, continuing program load"));
  }
  nextionSendCmd("page " + activePage); //boot to the home page
  nextionSendCmd("sendxy=1"); //enable touch tracking
  pressTimer = millis() + dimScreenDuration; //start the last press timer based on user-defined time-out

  ArduinoOTA.setPort(OTAport);
  ArduinoOTA.setHostname(SENSORNAME);
  ArduinoOTA.setPassword((const char *)OTApassword);

  debugPrintln("Starting Node named " + String(SENSORNAME));

  setup_wifi();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  ArduinoOTA.onStart([]() {
    debugPrintln("Starting");
  });
  ArduinoOTA.onEnd([]() {
    debugPrintln("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    //Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    debugPrintln("Progress: " + (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    //Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) debugPrintln("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) debugPrintln("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) debugPrintln("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) debugPrintln("Receive Failed");
    else if (error == OTA_END_ERROR) debugPrintln("End Failed");
  });
  ArduinoOTA.begin();
  debugPrintln("Ready");
  //debugPrint("IPess: ");
  //debugPrintln(String(WiFi.localIP()));
  reconnect();
}

/********************************** START SETUP WIFI*****************************************/
void setup_wifi() {

  pinMode(BUZZERPIN, OUTPUT);

  delay(10);
  debugPrintln("Connecting to " + String(wifi_ssid));

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid, wifi_password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    debugPrint(".");
  }

  debugPrintln("WiFi connected");
  //debugPrint("IP address: ");
  //debugPrintln(String(WiFi.localIP()));
}

/********************************** START CALLBACK*****************************************/
void callback(char* topic, byte* payload, unsigned int length) {
  debugPrintln("Message arrived [" + String(topic) + "] ");

  char message[length + 1];
  for (int i = 0; i < length; i++) {
    message[i] = (char)payload[i];
  }
  message[length] = '\0';
  debugPrintln(message);
  
  if (String(topic) == "home/alarm") { //if alarm state changes then can take message literally
    debugPrintln("Alarm status updating...");
    alarmStatus_old = alarmStatus;
    alarmStatus = message;
    if (alarmStatus == "pending") {
      pendingElapsed = millis();
    }
    if (activePage == "home") {
      updateHome();
    }
    if (activePage == "alarm") {
      updateAlarm();
    }
  } else {
    if (!processJson(message)) {
      return;
    }
  }
  //sendState();
}

/********************************** START PROCESS JSON*****************************************/
bool processJson(char* message) {
  StaticJsonBuffer<BUFFER_SIZE> jsonBuffer;

  JsonObject& root = jsonBuffer.parseObject(message);

  if (!root.success()) {
    debugPrintln("parseObject() failed");
    return false;
  }

  if (root.containsKey("state")) {
    if (strcmp(root["state"], on_cmd) == 0) {
      stateOn = true;
    }
    else if (strcmp(root["state"], off_cmd) == 0) {
      stateOn = false;
    }
  }

  if (root.containsKey("outsideTemp")) {
    outsideTemp = root["outsideTemp"].as<String>();
  }
  if (root.containsKey("insideTemp")) {
    insideTemp = root["insideTemp"].as<String>();
  }
  if (root.containsKey("rainfall")) {
    rainfall = root["rainfall"].as<String>();
  }
  if (root.containsKey("lights")) {
        if (strcmp(root["lights"], on_cmd) == 0) {
      lightsOn = true;
    }
    else if (strcmp(root["lights"], off_cmd) == 0) {
      lightsOn = false;
    }
    sendState(); //update status of our boolean_sensor in HA.
  }

  if (activePage == "home") {
    updateHome();
  }
  if (activePage == "alarm") {
    updateAlarm();
  }

  return true;
}

/********************************** START SEND STATE*****************************************/
void sendState() {
  StaticJsonBuffer<BUFFER_SIZE> jsonBuffer;

  JsonObject& root = jsonBuffer.createObject();

  root["state"] = (stateOn) ? on_cmd : off_cmd;
  root["lights"] = (lightsOn) ? on_cmd : off_cmd;
  
  char buffer[root.measureLength() + 1];
  root.printTo(buffer, sizeof(buffer));

  debugPrintln(buffer);
  client.publish(panel_state_topic, buffer, true);

  if (alarmStatus != alarmStatus_old) { //if we have changed status then send update message.
    char buffer[alarmStatus.length() + 1];
    alarmStatus.toCharArray(buffer, sizeof(buffer));

    debugPrintln("Updating topic [" + String(alarm_state_topic) + "] with value:" + buffer);
    client.publish(alarm_command_topic, buffer, true);
  }
}

void loop() {
  // put your main code here, to run repeatedly:
  ArduinoOTA.handle();

  if (!client.connected()) {
    // reconnect();
    software_Reset();
  }
  client.loop();

  if (alarmStatus == "pending") {
    //make beep, shouldn't be inputs to process.
    if ((millis() - pendingElapsed) > 1000) { //one second delay that allows other stuff to happen
      tone(BUZZERPIN, 1175, 250);
      delay(100);
      noTone(BUZZERPIN);
      pendingElapsed = millis();
    }    
  }

  if (nextionHandleInput())
  { // Process user input from HMI
    nextionProcessInput();
  }

  //check to see if we should dim the screen
  if ((pressTimer < millis()) && (!dimmed)) {
    nextionSendCmd("dim=" + dimmedValue);
    dimmed = true;
  }
}


/********************************** START RECONNECT*****************************************/
void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    debugPrintln("Attempting MQTT connection...");
    // Attempt to connect
    if (client.connect(SENSORNAME, mqtt_user, mqtt_password)) {
      debugPrintln("connected");
      client.subscribe(panel_command_topic);
      client.subscribe(alarm_state_topic);
      stateOn = true;
      sendState();
    } else {
      debugPrintln("failed, rc=");
      debugPrintln(String(client.state()));
      debugPrintln(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

/****reset***/
void software_Reset() // Restarts program from beginning but does not reset the peripherals and registers
{
  Serial.print("resetting");
  ESP.reset();
}

bool nextionHandleInput()
{ // Handle incoming serial data from the Nextion panel
  // This will collect serial data from the panel and place it into the global buffer
  // nextionReturnBuffer[nextionReturnIndex]
  // Return: true if we've received a string of 3 consecutive 0xFF values
  // Return: false otherwise
  bool nextionCommandComplete = false;
  static int nextionTermByteCnt = 0;   // counter for our 3 consecutive 0xFFs
  static String hmiDebug = "HMI IN: "; // assemble a string for debug output

  if (Serial.available())
  {
    lcdConnected = true;
    byte nextionCommandByte = Serial.read();
    hmiDebug += (" 0x" + String(nextionCommandByte, HEX));
    // check to see if we have one of 3 consecutive 0xFF which indicates the end of a command
    if (nextionCommandByte == 0xFF)
    {
      nextionTermByteCnt++;
      if (nextionTermByteCnt >= 3)
      { // We have received a complete command
        nextionCommandComplete = true;
        nextionTermByteCnt = 0; // reset counter
      }
    }
    else
    {
      nextionTermByteCnt = 0; // reset counter if a non-term byte was encountered
    }
    nextionReturnBuffer[nextionReturnIndex] = nextionCommandByte;
    nextionReturnIndex++;
  }
  if (nextionCommandComplete)
  {
    debugPrintln(hmiDebug);
    hmiDebug = "HMI IN: ";
  }
  return nextionCommandComplete;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
void nextionProcessInput()
{ // Process incoming serial commands from the Nextion panel
  // Command reference: https://www.itead.cc/wiki/Nextion_Instruction_Set#Format_of_Device_Return_Data
  // tl;dr, command byte, command data, 0xFF 0xFF 0xFF
  if (nextionReturnBuffer[0] == 0x67) { //Handle incoming touch coordinate command
    int nextionX_H = int(nextionReturnBuffer[1]);
    int nextionX_L = int(nextionReturnBuffer[2]);
    int nextionY_H = int(nextionReturnBuffer[3]);
    int nextionY_L = int(nextionReturnBuffer[4]);
    byte nextionButtonAction = nextionReturnBuffer[5];

    nextionSendCmd("dim=100"); //button has been pressed to light up the display to max.
    dimmed = false; //reset boolean
    pressTimer = millis() + (dimScreenDuration * 1000); //reset the last press timer and set a new one based on user-defined time-out
    
    //emulation of swipe based on duration of press and start point, combined with button area on each page it should be intuitive
    if (nextionButtonAction == 0x01) { //press event
      longPress = false; //reset our boolean to allow for normal presses to recommence
      debugPrintln("X_H=" + String(nextionX_H) + ", X_L=" + String(nextionX_L) +",Y_H=" + String(nextionY_H) +",Y_L=" + String(nextionY_L) + ".");
      
      if (((nextionX_H + nextionX_L) > 160) && (activePage == "home")) { // if press is in left one-third of home screen then set timer
        transitionTimer = millis() + 500; //add 500 milliseconds as our threshold
      } else if (((nextionX_H + nextionX_L) < 80) && (activePage == "alarm")) { // if press is in right one-third of alarm screen then set timer
        transitionTimer = millis() + 500; //add 500 milliseconds as our threshold
      } else {
        transitionTimer = 0;
      }
      
    }
    if (nextionButtonAction == 0x00) { //release event, must be before the button press code so we can ignore a button press if required
      debugPrintln("X_H=" + String(nextionX_H) + ", X_L=" + String(nextionX_L) +",Y_H=" + String(nextionY_H) +",Y_L=" + String(nextionY_L) + ".");
      if ((transitionTimer > 0) && (transitionTimer < millis())) {
        debugPrintln("Transition command detected.");
        longPress = true;
        //just make a clicking sound
        digitalWrite(BUZZERPIN, HIGH);
        delay(150);
        digitalWrite(BUZZERPIN, LOW);
        if (activePage == "home") {
          updateAlarm(); 
        } else {
          updateHome();
        }
      }
    }
  }

  
  if ((nextionReturnBuffer[0] == 0x65) && (!longPress))
  { // Handle incoming touch command
    // 0x65+Page ID+Component ID+TouchEvent+End
    // Return this data when the touch event created by the user is pressed.
    // Definition of TouchEvent: Press Event 0x01, Release Event 0X00
    // Example: 0x65 0x00 0x02 0x01 0xFF 0xFF 0xFF
    // Meaning: Touch Event, Page 0, Object 2, Press
    String nextionPage = String(nextionReturnBuffer[1]);
    String nextionButtonID = String(nextionReturnBuffer[2]);
    byte nextionButtonAction = nextionReturnBuffer[3];

    if (nextionButtonAction == 0x00)
    {
      debugPrintln(String(F("HMI IN: [Button PRESS] 'p[")) + nextionPage + "].b[" + nextionButtonID + "]'");
      if (nextionPage == "0") { //Home page
        activePage = "home"; //reset status based on actual screen value in case there's been an issue.
        if (nextionButtonID == "1") { // ARM/DISARM ALARM BUTTON
          if (alarmStatus == "disarmed") {
            tone(BUZZERPIN, 1568, 250);
            delay(150);
            noTone(BUZZERPIN);
            //tone(BUZZERPIN, 3136, 250);
            //delay(150);
            //noTone(BUZZERPIN);
            // if unarmed then set to arming
            alarmStatus_old = alarmStatus;
            alarmStatus = "armed_away";
            sendState();
          } else {
            //just make a clicking sound
            digitalWrite(BUZZERPIN, HIGH);
            delay(150);
            digitalWrite(BUZZERPIN, LOW);
            // if armed then move to alarm page
            updateAlarm();
          }
        }

        if (nextionButtonID == "6") { //lights button, only active if lights are on
          tone(BUZZERPIN, 1568, 250);
          delay(150);
          noTone(BUZZERPIN);
          delay(150);
          tone(BUZZERPIN, 880, 250);
          delay(150);
          noTone(BUZZERPIN);
          delay(150);
          tone(BUZZERPIN, 123, 250);
          delay(150);
          noTone(BUZZERPIN);
          lightsOn = false;
          sendState();
        }

        if (nextionButtonID == "5") { //moving forward a page
          //just make a clicking sound
          digitalWrite(BUZZERPIN, HIGH);
          delay(150);
          digitalWrite(BUZZERPIN, LOW);
          updateAlarm();
        }
      }

      if (nextionPage == "1") { //Alarm page
        activePage = "alarm"; //reset status based on actual screen value in case there's been an issue.
        if (nextionButtonID.toInt() <= 10) {
          //sound buzzer
          tone(BUZZERPIN, int(nextionButtonID.toInt() * 375), 125);
          delay(150);
          noTone(BUZZERPIN);
          if (press_count >= 4) { //complete reset to avoid partial codes being stored
            alarmCode[0] = 0;
            alarmCode[1] = 0;
            alarmCode[2] = 0;
            alarmCode[3] = 0;
            press_count = 0;
          }
          alarmCode[press_count] = nextionButtonID.toInt() - 1; //our buttons have ids running from 1 to 10 according their designation
          press_count++;
          codeWidth = int(147 * press_count * 0.25);
          codeWidthWipe = 147 - codeWidth;
          nextionSendCmd("picq 85,198," + String(codeWidth) + ",65,5");
          nextionSendCmd("picq " + String(codeWidth + 85) + ",198," + String(codeWidthWipe) + ",65,4"); //overwrite with negative image
        }

        if (nextionButtonID == "11") { // ARM/DISARM ALARM BUTTON
          debugPrintln("Button 11 pressed, alarm value is: " + alarmStatus);
          if (alarmStatus == "disarmed") {
            tone(BUZZERPIN, 1568, 250);
            delay(150);
            noTone(BUZZERPIN);
            //tone(BUZZERPIN, 3136, 250);
            //delay(150);
            //noTone(BUZZERPIN);
            // if unarmed then set to arming
            alarmStatus_old = alarmStatus;
            alarmStatus = "armed_away";
            sendState();
          } else {
            if ((String(alarmCode[0]) + String(alarmCode[1]) + String(alarmCode[2]) + String(alarmCode[3])) == masterCode) {
              tone(BUZZERPIN, 1568, 250);
              delay(150);
              noTone(BUZZERPIN);
              tone(BUZZERPIN, 2637, 250);
              delay(150);
              noTone(BUZZERPIN);
              tone(BUZZERPIN, 3520, 250);
              delay(150);
              noTone(BUZZERPIN);
              alarmStatus_old = alarmStatus;
              alarmStatus = "disarmed"; // if armed then set to not armed
              sendState();
              updateHome(); //move to home page
            } else {
              tone(BUZZERPIN, 131, 250);
              delay(150);
              noTone(BUZZERPIN);
              tone(BUZZERPIN, 143, 250);
              delay(150);
              noTone(BUZZERPIN);
            }
            //reset the code whether right or wrong
            alarmCode[0] = 0;
            alarmCode[1] = 0;
            alarmCode[2] = 0;
            alarmCode[3] = 0;
            press_count = 0;
            nextionSendCmd("picq 85,198,0,65,5");
            nextionSendCmd("picq 85,198,147,65,4"); //clear
          }
        }

        if (nextionButtonID == "12") { //going back a page
          //just make a clicking sound
          digitalWrite(BUZZERPIN, HIGH);
          delay(150);
          digitalWrite(BUZZERPIN, LOW);
          updateHome();
        }
      }
    }
  }
  nextionReturnIndex = 0; // Done handling the buffer, reset index back to 0
}

void updateHome() {
  nextionSendCmd("page home");
  activePage = "home";
  nextionSendCmd("t0.txt=\"" + insideTemp + "°C\"");
  nextionSendCmd("t1.txt=\"" + outsideTemp + "°C\"");
  nextionSendCmd("t2.txt=\"" + rainfall + "mm\"");
  if (lightsOn) { //change from default if lights are on
    nextionSendCmd("b1.picc=3");
    nextionSendCmd("b1.picc2=1");
    nextionSendCmd("tsw b1,1"); //enable touch while it's in this mode
  }

  if (alarmStatus == "pending") {
    nextionSendCmd("b0.picc=2");
    nextionSendCmd("tsw b0,0"); //disable touch while it's in this mode
  } else if (alarmStatus == "armed_away") {
    nextionSendCmd("b0.picc=3");
    nextionSendCmd("tsw b0,1"); //enable touch while it's in this mode
  } else { //assume disarmed
    nextionSendCmd("b0.picc=0");
    nextionSendCmd("tsw b0,1"); //enable touch while it's in this mode
    //send disarmed message if we don't recognise the status
    if (alarmStatus != "disarmed") {
      alarmStatus_old = alarmStatus;
      alarmStatus = "disarmed";
      sendState();
    }
  }

  if (lightsOn) {
    nextionSendCmd("b1.picc=3");
    nextionSendCmd("b1.picc2=1");
    nextionSendCmd("tsw b1,1"); //enable touch while it's in this mode
  } else {
    nextionSendCmd("b1.picc=0");
    nextionSendCmd("b1.picc2=0"); //not needed but for clarity
    nextionSendCmd("tsw b1,0"); //disable touch while it's in this mode
  }
}

void updateAlarm() {
  nextionSendCmd("page alarm");
  activePage = "alarm";
  if (alarmStatus == "pending") {
    nextionSendCmd("b10.picc=6");
    nextionSendCmd("tsw b10,0");
  } else if (alarmStatus == "armed_away") { //default is unarmed
    nextionSendCmd("b10.picc=7");
    nextionSendCmd("b10.picc2=7");
    nextionSendCmd("tsw b10,1"); //enable touch while it's in this mode
  } else { // assumed disarmed
    nextionSendCmd("b10.picc=4");
    nextionSendCmd("b10.picc2=5");
    nextionSendCmd("tsw b10,1"); //enable touch while it's in this mode
    //send disarmed message if we don't recognise the status
    if (alarmStatus != "disarmed") {
      alarmStatus_old = alarmStatus;
      alarmStatus = "disarmed";
      sendState();
    }
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
void nextionGetAttr(String hmiAttribute)
{ // Get the value of a Nextion component attribute
  // This will only send the command to the panel requesting the attribute, the actual
  // return of that value will be handled by nextionProcessInput and placed into mqttGetSubtopic
  Serial1.print("get " + hmiAttribute);
  Serial1.write(nextionSuffix, sizeof(nextionSuffix));
  debugPrintln(String(F("HMI OUT: 'get ")) + hmiAttribute + "'");
}

////////////////////////////////////////////////////////////////////////////////////////////////////
void nextionSendCmd(String nextionCmd)
{ // Send a raw command to the Nextion panel
  Serial1.print(utf8ascii(nextionCmd));
  Serial1.write(nextionSuffix, sizeof(nextionSuffix));
  debugPrintln(String(F("HMI OUT: ")) + nextionCmd);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
void debugPrintln(String debugText)
{ // Debug output line of text to our debug targets
  String debugTimeText = "[+" + String(float(millis()) / 1000, 3) + "s] " + debugText;
  Serial.println(debugTimeText);
  if (debugSerialEnabled)
  {
    SoftwareSerial debugSerial(SW_SERIAL_UNUSED_PIN, 1); // 17==nc for RX, 1==TX pin
    debugSerial.begin(115200);
    debugSerial.println(debugTimeText);
    debugSerial.flush();
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
void debugPrint(String debugText)
{ // Debug output single character to our debug targets (DON'T USE THIS!)
  // Try to avoid using this function if at all possible.  When connected to telnet, printing each
  // character requires a full TCP round-trip + acknowledgement back and execution halts while this
  // happens.  Far better to put everything into a line and send it all out in one packet using
  // debugPrintln.
  if (debugSerialEnabled)
    Serial.print(debugText);
  {
    SoftwareSerial debugSerial(SW_SERIAL_UNUSED_PIN, 1); // 17==nc for RX, 1==TX pin
    debugSerial.begin(115200);
    debugSerial.print(debugText);
    debugSerial.flush();
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// UTF8-Decoder: convert UTF8-string to extended ASCII http://playground.arduino.cc/main/Utf8ascii
// Workaround for issue here: https://github.com/home-assistant/home-assistant/issues/9528
// Nextion claims that "Unicode and UTF will not be among the supported encodings", so this should
// be safe to run against all attribute values coming in.
static byte c1; // Last character buffer
byte utf8ascii(byte ascii)
{ // Convert a single Character from UTF8 to Extended ASCII. Return "0" if a byte has to be ignored.
  if (ascii < 128)
  { // Standard ASCII-set 0..0x7F handling
    c1 = 0;
    return (ascii);
  }
  // get previous input
  byte last = c1; // get last char
  c1 = ascii;     // remember actual character
  switch (last)
  { // conversion depending on first UTF8-character
    case 0xC2:
      return (ascii);
      break;
    case 0xC3:
      return (ascii | 0xC0);
      break;
    case 0x82:
      if (ascii == 0xAC)
        return (0x80); // special case Euro-symbol
  }
  return (0); // otherwise: return zero, if character has to be ignored
}

String utf8ascii(String s)
{ // convert String object from UTF8 String to Extended ASCII
  String r = "";
  char c;
  for (uint16_t i = 0; i < s.length(); i++)
  {
    c = utf8ascii(s.charAt(i));
    if (c != 0)
      r += c;
  }
  return r;
}

void utf8ascii(char *s)
{ // In Place conversion UTF8-string to Extended ASCII (ASCII is shorter!)
  uint16_t k = 0;
  char c;
  for (uint16_t i = 0; i < strlen(s); i++)
  {
    c = utf8ascii(s[i]);
    if (c != 0)
      s[k++] = c;
  }
  s[k] = 0;
}
