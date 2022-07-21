#include <Arduino.h>          // Basic Needs
#include <Wire.h>
#include <SPI.h>
#include <StrokeEngine.h>     // Include Stroke Engine
#include "OSSM_Config.h"      // START HERE FOR Configuration
#include "OSSM_PinDEF.h"      // This is where you set pins specific for your board
#include "FastLED.h"          // Used for the LED on the Reference Board (or any other pixel LEDS you may add)
#include "RotaryEncoder.h"
#include "OssmUi.h"           // Separate file that helps contain the OLED screen functions for Local Remotes
#include <esp_now.h>
#include <WiFi.h>
#include "ModbusClientRTU.h"
#include "OneButton.h"
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>


#define BTN_NONE   0
#define BTN_SHORT  1
#define BTN_LONG   2
#define BTN_V_LONG 3

#define CONN 0
#define SPEED 1
#define DEPTH 2
#define STROKE 3
#define SENSATION 4
#define PATTERN 5
#define TORQE_F 6
#define TORQE_R 7
#define OFF 10
#define ON   11
#define SETUP_D_I 12
#define SETUP_D_I_F 13
#define REBOOT 14

OneButton ALM(SERVO_ALM_PIN, false);
OneButton PED(SERVO_PED_PIN, false);

volatile float speedPercentage = 0;
volatile float sensation = 0;

// Variable to store if sending data was successful
String success;

float out_esp_speed;
float out_esp_depth;
float out_esp_stroke;
float out_esp_sensation;
float out_esp_pattern;
bool out_esp_rstate;
bool out_esp_connected;
int out_esp_command;
float out_esp_value;

float incoming_esp_speed;
float incoming_esp_depth;
float incoming_esp_stroke;
float incoming_esp_sensation;
float incoming_esp_pattern;
bool incoming_esp_rstate;
bool incoming_esp_connected;
int incoming_esp_command;
float incoming_esp_value;

typedef struct struct_message {
  float esp_speed;
  float esp_depth;
  float esp_stroke;
  float esp_sensation;
  float esp_pattern;
  bool esp_rstate;
  bool esp_connected;
  int esp_command;
  float esp_value;
} struct_message;

bool esp_connect = false;

struct_message outgoingcontrol;
struct_message incomingcontrol;

esp_now_peer_info_t peerInfo;
ModbusClientRTU MB(Serial2);
uint32_t Token = 1111;


///////////////////////////////////////////
////
////  RTOS Settings
////
///////////////////////////////////////////

#define configSUPPORT_DYNAMIC_ALLOCATION    1
#define INCLUDE_uxTaskGetStackHighWaterMark 1

//Set Values to 0
float speed     = 0.0;
float depth     = 0.0;
float stroke    = 0.0;
float zero      = 0.0;
int pattern = 0;

RotaryEncoder *encoder;

// State Machine Local Remote
enum States {START, HOME, M_MENUE, M_SET_DEPTH, M_SET_STROKE, M_SET_SENSATION, M_SET_PATTERN, M_SET_DEPTH_INT, M_SET_DEPTH_FANCY, OPT_SET_DEPTH, OPT_SET_STROKE, OPT_SET_SENSATION, OPT_SET_PATTERN, OPT_SET_DEPTH_INT, OPT_SET_DEPTH_FANCY};
uint8_t state = START;

// Display LocaL Remote
OssmUi g_ui(REMOTE_ADDRESS, REMOTE_SDA, REMOTE_CLK);


static motorProperties servoMotor {
  .maxSpeed = MAX_SPEED,                // Maximum speed the system can go in mm/s
  .maxAcceleration = MAX_ACCELERATION,  // Maximum linear acceleration in mm/s²
  .stepsPerMillimeter = STEP_PER_MM,    // Steps per millimeter 
  .invertDirection = true,              // One of many ways to change the direction,  
                                        // should things move the wrong way
  .enableActiveLow = true,              // Polarity of the enable signal      
  .stepPin = SERVO_PULSE,               // Pin of the STEP signal
  .directionPin = SERVO_DIR,            // Pin of the DIR signal
  .enablePin = SERVO_ENABLE             // Pin of the enable signal
};

static machineGeometry strokingMachine = {
  .physicalTravel = MAX_STROKEINMM,            // Real physical travel from one hard endstop to the other
  .keepoutBoundary = STROKEBOUNDARY              // Safe distance the motion is constrained to avoiding crashes
};

// Configure Homing Procedure
static endstopProperties endstop = {
  .homeToBack = true,                // Endstop sits at the rear of the machine
  .activeLow = true,                  // switch is wired active low
  .endstopPin = SERVO_ENDSTOP,        // Pin number
  .pinMode = INPUT_PULLUP             // pinmode INPUT with external pull-up resistor
};

StrokeEngine Stroker;

///////////////////////////////////////////
////
////  To Debug or not to Debug
////
///////////////////////////////////////////

// Uncomment the following line if you wish to print DEBUG info
#define DEBUG 

#ifdef DEBUG
#define LogDebug(...) Serial.println(__VA_ARGS__)
#define LogDebugFormatted(...) Serial.printf(__VA_ARGS__)
#else
#define LogDebug(...) ((void)0)
#define LogDebugFormatted(...) ((void)0)
#endif

// Create tasks for checking pot input or web server control, and task to handle
// planning the motion profile (this task is high level only and does not pulse
// the stepper!)

TaskHandle_t bldedisc_t = nullptr;
TaskHandle_t estop_T    = nullptr;  // Estop Taks for Emergency 
TaskHandle_t CRemote_T  = nullptr;  // Cable Remote Task 
TaskHandle_t eRemote_t  = nullptr;  // Esp Now Remote

#define BRIGHTNESS 170
#define LED_TYPE WS2811
#define COLOR_ORDER GRB
#define LED_PIN 25
#define NUM_LEDS 1
CRGB leds[NUM_LEDS];

///////////////////////////////////////////
////
////  Bluetooth
////
///////////////////////////////////////////

// We use an Enum to define the Mode of our Device
enum DeviceMode {
  Waiting, // Not discovering, not timed out
  Discovering, // We're in Discovery mode
  Discovered,  // Discovery Succeeded
  Failed,  // Discovery Failed (Timed Out)
};

DeviceMode deviceMode = Waiting; // We are initially Waiting

BLEServer *bleServer;
BLEService *bleService;
BLECharacteristic *bleCharacteristic;
BLEAdvertising *bleAdvertising;
bool bleClientConnected = false;
unsigned long discoveredAt;

class BLECallbacks: public BLEServerCallbacks {
   void onConnect(BLEServer* pServer) {
      Serial.println("BLE Client Connected!");
      bleClientConnected = true;
    };

    void onDisconnect(BLEServer* pServer) {
      Serial.println("BLE Client Disconnected!");
      bleClientConnected = false;
      deviceMode = Discovered;
      discoveredAt = 0;
      vTaskDelete(bldedisc_t);
    } 
};

///////////////////////////////////////////
////
////  Tasks
////
///////////////////////////////////////////


// Declarations
void blediscovery(void *pvParameters); //Handles 
void emergencyStopTask(void *pvParameters); // Handels all Higher Emergency Stop Functions
void CableRemoteTask(void *pvParameters);  // Handels all Functions from Cable Remote
void espNowRemoteTask(void *pvParameters); // Handels the EspNow Remote
void setLedRainbow(CRGB leds[]);
void almclick();
void pedclick();

float getAnalogAverage(int pinNumber, int samples);


// Homing Feedback Serial
void homingNotification(bool isHomed) {
  if (isHomed) {
    LogDebug("Found home - Ready to rumble!");
    g_ui.UpdateMessage("Homed - Ready to rumble!");
  } else {
    g_ui.UpdateMessage("Homing failed!");
    LogDebug("Homing failed!");
  }
}

// Mobus for RS232
void handleData(ModbusMessage msg, uint32_t token){
  Serial.printf("Response: serverID=%d, FC=%d, Token=%08X, length=%d:\n", msg.getServerID(), msg.getFunctionCode(), token, msg.size());
  for (auto& byte : msg) {
    Serial.printf("%02X ", byte);
  Serial.println("");
}
}

void handleError(Error error, uint32_t token){
  // ModbusError wraps the error code and provides a readable error message for it
  ModbusError me(error);
  Serial.printf("Error response: %02X - %s\n", error, (const char *)me);
}

// Callback when data is sent
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("\r\nLast Packet Send Status:\t");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
  if (status ==0){
    success = "Delivery Success :)";
  }
  else{
    success = "Delivery Fail :(";
  }
}

// Callback when data is received
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
  memcpy(&incomingcontrol, incomingData, sizeof(incomingcontrol));
  if(esp_connect == false && incomingcontrol.esp_connected == false){
    switch(incomingcontrol.esp_command)
    {
      case REBOOT:
      ESP.restart();
      break; 
    }
    outgoingcontrol.esp_connected = true;
    outgoingcontrol.esp_speed = USER_SPEEDLIMIT;
    outgoingcontrol.esp_depth = MAX_STROKEINMM;
    outgoingcontrol.esp_pattern = Stroker.getPattern();
    esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *) &outgoingcontrol, sizeof(outgoingcontrol));
    if (result == ESP_OK) {
      esp_connect = true;
    }
  } else if(esp_connect == true && incomingcontrol.esp_connected == false){
    Stroker.disable();
    Stroker.enableAndHome(&endstop, homingNotification);
    outgoingcontrol.esp_connected = true;
    outgoingcontrol.esp_speed = USER_SPEEDLIMIT;
    outgoingcontrol.esp_depth = MAX_STROKEINMM;
    outgoingcontrol.esp_pattern = Stroker.getPattern();
    esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *) &outgoingcontrol, sizeof(outgoingcontrol));
    if (result == ESP_OK) {
      esp_connect = true;
    }
  } else if(esp_connect == true && incomingcontrol.esp_connected == true){
    LogDebug(incomingcontrol.esp_command);
    LogDebug(incomingcontrol.esp_value);
    switch(incomingcontrol.esp_command)
    {
      case ON:
      Stroker.startPattern();
      break;
      case OFF:
      Stroker.stopMotion();
      break;
      case SPEED:
      speed = incomingcontrol.esp_value; 
      Stroker.setSpeed(speed, true);
      break;
      case DEPTH:
      depth = incomingcontrol.esp_value;
      Stroker.setDepth(depth, true);
      break;
      case STROKE:
      stroke = incomingcontrol.esp_value;
      Stroker.setStroke(stroke, true);
      break;
      case SENSATION:
      sensation = incomingcontrol.esp_value;
      Stroker.setSensation(sensation, true);
      break;
      case PATTERN:
      {
      int patter = incomingcontrol.esp_value;
      Stroker.setPattern(patter, true);
      LogDebug(Stroker.getPatternName(patter));
      }
      break;
      case TORQE_F:
      {
        int torqe = incomingcontrol.esp_value * 10;
        LogDebug(torqe);
        Error err = MB.addRequest(Token++, 1, WRITE_HOLD_REGISTER, 0x01FE, torqe);
        if (err!=SUCCESS) {
        ModbusError e(err);
        Serial.printf("Error creating request: %02X - %s\n", (int)e, (const char *)e);
        }
      }
      break;
      case TORQE_R:
      {
        int torqe = 65535 - (incomingcontrol.esp_value * -10);
        LogDebug(torqe);
        Error err = MB.addRequest(Token++, 1, WRITE_HOLD_REGISTER, 0x01FF, torqe);
        if (err!=SUCCESS) {
        ModbusError e(err);
        Serial.printf("Error creating request: %02X - %s\n", (int)e, (const char *)e);
        }
      }
      break;
      case SETUP_D_I:
      Stroker.setupDepth(10, false);
      break;
      case SETUP_D_I_F:
      Stroker.setupDepth(10, true);
      break;
      case REBOOT:
      ESP.restart();
      break;     
    }
  }
}

void setup() {
  Serial.begin(115200);         // Start Serial.
  Serial2.begin(57600, SERIAL_8E1, GPIO_NUM_16, GPIO_NUM_17);
  LogDebug("\n Starting");      // Start LogDebug
  delay(200);
  
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(150);
  setLedRainbow(leds);
  FastLED.show();
  BLEDevice::init("OSSM");

  MB.onDataHandler(&handleData);
  MB.onErrorHandler(&handleError);
  MB.setTimeout(2000);
  MB.begin();
  
  Error err = MB.addRequest(Token++, 1, READ_HOLD_REGISTER, 0x01FE, 1);
  if (err!=SUCCESS) {
  ModbusError e(err);
  Serial.printf("Error creating request: %02X - %s\n", (int)e, (const char *)e);
  }

  // OLED SETUP
  g_ui.Setup();
  g_ui.UpdateOnly();

  Stroker.begin(&strokingMachine, &servoMotor);     // Setup Stroke Engine
  Stroker.enableAndHome(&endstop, homingNotification);    // pointer to the homing config struct

  //Start Tasks here:
  xTaskCreatePinnedToCore(emergencyStopTask,     /* Task function. */
                            "emergencyStopTask", /* name of task. */
                            2048,                /* Stack size of task */
                            NULL,                /* parameter of the task */
                            1,                   /* priority of the task */
                            &estop_T,            /* Task handle to keep track of created task */
                            0);                  /* pin task to core 0 */
  delay(100);

  xTaskCreatePinnedToCore(CableRemoteTask,      /* Task function. */
                            "CableRemoteTask",  /* name of task. */
                            4096,               /* Stack size of task */
                            NULL,               /* parameter of the task */
                            5,                  /* priority of the task */
                            &CRemote_T,         /* Task handle to keep track of created task */
                            0);                 /* pin task to core 0 */
  delay(100);
  
  xTaskCreatePinnedToCore(espNowRemoteTask,      /* Task function. */
                            "espNowRemoteTask",  /* name of task. */
                            4096,               /* Stack size of task */
                            NULL,               /* parameter of the task */
                            5,                  /* priority of the task */
                            &eRemote_t,         /* Task handle to keep track of created task */
                            0);                 /* pin task to core 0 */
  delay(100);

  xTaskCreatePinnedToCore(blediscovery,      /* Task function. */
                            "blediscovery",  /* name of task. */
                            8096,               /* Stack size of task */
                            NULL,               /* parameter of the task */
                            5,                  /* priority of the task */
                            &bldedisc_t,         /* Task handle to keep track of created task */
                            0);                 /* pin task to core 0 */
  delay(100);
  vTaskSuspend(bldedisc_t);

  

  if(!g_ui.DisplayIsConnected()){
    vTaskSuspend(CRemote_T);
  }
  


  pinMode(SERVO_ALM_PIN, INPUT);
  pinMode(SERVO_PED_PIN, INPUT);
  //ALM.attachClick(almclick);
  //PED.attachClick(pedclick);

  pinMode(SPEED_POT_PIN, INPUT);
  adcAttachPin(SPEED_POT_PIN);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db); // allows us to read almost full 3.3V range
  
  // wait for homing to complete
  while (Stroker.getState() != READY) {
    delay(100);
  }
  Stroker.setSpeed(0.0, true);
  Stroker.setDepth(0.0, true);
  Stroker.setStroke(0.0, true);

    BLEDevice::init("OSSM");
  if (bleServer == nullptr) {
    Serial.println("First Time Discovering");
    // Get the MAC Address
    WiFi.mode(WIFI_STA);
    uint8_t mac[6];
    if(WiFiGenericClass::getMode() == WIFI_MODE_NULL){
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }

    // Prepare our BLE Server
    bleServer = BLEDevice::createServer();
    bleServer->setCallbacks(new BLECallbacks());

    // Prepare our Service
    bleService = bleServer->createService(UUID_SERVICE);

    // A Characteristic is what we shall use to provide Clients/Slaves with our MAC Address.
    bleCharacteristic = bleService->createCharacteristic(UUID_CHARACTERISTIC, BLECharacteristic::PROPERTY_READ);

    // Provide our Characteristic with the MAC Address "Payload"
    bleCharacteristic->setValue(&mac[0], 6);
    // Make the Property visible to Clients/Slaves.
    bleCharacteristic->setBroadcastProperty(true);

    // Start the BLE Service
    bleService->start();
  
    // Advertise it!
    bleAdvertising = BLEDevice::getAdvertising();
    bleAdvertising->addServiceUUID(UUID_SERVICE);
    bleAdvertising->setScanResponse(true);
    bleAdvertising->setMinPreferred(0x06);
    bleAdvertising->setMinPreferred(0x12);
    BLEDevice::startAdvertising();
  }
  
  // Start the BLE Service
  bleService->start();

  // Advertise it!
  bleAdvertising = BLEDevice::getAdvertising();
  BLEDevice::startAdvertising();
}

void loop() {
  g_ui.UpdateScreen();
  //PED.tick();
  //ALM.tick();
}

void blediscovery(void *pvParameters)
{
for (;;)
{
}
vTaskDelay(200);
}

void emergencyStopTask(void *pvParameters)
{
 for (;;)
  { 
    //bool alm = digitalRead(SERVO_ALM_PIN);
    //bool ped = digitalRead(SERVO_PED_PIN);
    //LogDebugFormatted("ALM: %ld \n", static_cast<long int>(alm));
    //LogDebugFormatted("PED: %ld \n", static_cast<long int>(ped));
    static bool is_connected = false;
    if (!is_connected && g_ui.DisplayIsConnected())
    {
        LogDebug("Display Connected");
        is_connected = true;
        vTaskResume(CRemote_T);
    }
    else if (is_connected && !g_ui.DisplayIsConnected())
    {
        LogDebug("Display Disconnected");
        is_connected = false;
        if(Stroker.getState() == PATTERN){
        Stroker.stopMotion();
        }
        vTaskSuspend(CRemote_T);

    }
    if(esp_connect == true){
      vTaskSuspend(CRemote_T);
    }
    vTaskDelay(200);
  }
}

void CableRemoteTask(void *pvParameters)
{
   encoder = new RotaryEncoder();
   int patternN = Stroker.getNumberOfPattern();
   sensation = Stroker.getSensation();
   int buttonstate = 0;
    for (;;)
    {
      buttonstate = encoder->check_button();
      switch(state)
       {
          case START:
          if (buttonstate == BTN_LONG){
            state = HOME;
            g_ui.clearLogo();
            g_ui.UpdateMessage("Hold Down to Start");
            g_ui.UpdateTitelL("Speed");
            g_ui.UpdateTitelR("Sensation");
            sensation = Stroker.getSensation();
            g_ui.UpdateStateR(map(sensation,-100,100,0,100));
          break;
          }
          break;

          case HOME:

          if (buttonstate == BTN_V_LONG) {
            if(Stroker.getState() == PATTERN){
              Stroker.stopMotion();
            } else {
              Stroker.startPattern();
            }
          break;
          }

          if (buttonstate == BTN_LONG) {
              g_ui.UpdateMessage("Home");
              g_ui.UpdateTitelR("");
              g_ui.UpdateStateR(zero);
              state = M_MENUE;
          break;
          }

          if (encoder->wasTurnedLeft()) {
          sensation = constrain((sensation - (200/ENCODER_RESULTION)), -100, 100);
          Stroker.setSensation(sensation, false);
          g_ui.UpdateStateR(map(sensation,-100,100,0,100));
          break;
          } else if (encoder->wasTurnedRight()) {
          sensation = constrain((sensation + (200/ENCODER_RESULTION)), -100, 100);
          Stroker.setSensation(sensation, false);
          g_ui.UpdateStateR(map(sensation,-100,100,0,100));
          break;
          }
          break;

          case M_MENUE:
          if (buttonstate == BTN_LONG) {
            sensation = Stroker.getSensation();
            g_ui.UpdateMessage("Hold For Start");
            g_ui.UpdateTitelR("Sensation");
            g_ui.UpdateStateR(map(sensation,-100,100,0,100));
            state = HOME;
          break;
          }
          
          if (encoder->wasTurnedLeft()) {
          g_ui.UpdateMessage("Depth Fancy");
          state = M_SET_DEPTH_FANCY;
          break;
          } else if (encoder->wasTurnedRight()) {
          g_ui.UpdateMessage("Set Depth");  
          state = M_SET_DEPTH;
          break;
          }
          break;

          case M_SET_DEPTH:
          if (buttonstate == BTN_LONG) {
          state = OPT_SET_DEPTH;
          g_ui.UpdateMessage("->Set Depth<-");
          depth = Stroker.getDepth();
          g_ui.UpdateStateR(map(depth,0,MAX_STROKEINMM,0,100));
          g_ui.UpdateTitelR("Depth");
          break;
          }
          if (encoder->wasTurnedLeft()) {
          g_ui.UpdateMessage("Home");
          state = M_MENUE;
          break;
          } else if (encoder->wasTurnedRight()) {
          g_ui.UpdateMessage("Set Stroke");  
          state = M_SET_STROKE;
          break;
          }
          break;

          case OPT_SET_DEPTH:
          if (buttonstate == BTN_LONG) {
          state = M_SET_DEPTH;
          g_ui.UpdateMessage("Set Depth");
          g_ui.UpdateStateR(zero);
          g_ui.UpdateTitelR("");
          break;
          }

          if (encoder->wasTurnedLeft()) {
          depth = constrain((depth - DEPTH_RESULTION) , 0, MAX_STROKEINMM);
          Stroker.setDepth(depth, false);
          g_ui.UpdateStateR(map(depth,0,MAX_STROKEINMM,0,100));
          LogDebug(depth);
          break;
          } else if (encoder->wasTurnedRight()) {
          depth = constrain((depth + DEPTH_RESULTION) , 0, MAX_STROKEINMM);
          Stroker.setDepth(depth, false);
          g_ui.UpdateStateR(map(depth,0,MAX_STROKEINMM,0,100));
          LogDebug(depth);
          break;
          }
          break;

          case M_SET_STROKE:
          if (buttonstate == BTN_LONG) {
          state = OPT_SET_STROKE;
          g_ui.UpdateMessage("->Set Stroke<-");
          stroke = Stroker.getStroke();
          g_ui.UpdateStateR(map(stroke,0,MAX_STROKEINMM,0,100));
          g_ui.UpdateTitelR("Stroke");
          break;
          }
          if (encoder->wasTurnedLeft()) {
          g_ui.UpdateMessage("Set Depth");
          state = M_SET_DEPTH;
          break;
          } else if (encoder->wasTurnedRight()) {
          g_ui.UpdateMessage("Set Pattern");  
          state = M_SET_PATTERN;
          break;
          }
          break;

          case OPT_SET_STROKE:
          if (buttonstate == BTN_LONG) {
          state = M_SET_STROKE;
          g_ui.UpdateMessage("Set Stroke");
          g_ui.UpdateStateR(zero);
          g_ui.UpdateTitelR("");
          break;
          }

          if (encoder->wasTurnedLeft()) {
          stroke = constrain((stroke - STROKE_RESULTION) , 0, MAX_STROKEINMM);
          Stroker.setStroke(stroke, false);
          g_ui.UpdateStateR(map(stroke,0,MAX_STROKEINMM,0,100));
          LogDebug(stroke);
          break;
          } else if (encoder->wasTurnedRight()) {
          stroke = constrain((stroke + STROKE_RESULTION) , 0, MAX_STROKEINMM);
          Stroker.setStroke(stroke, false);
          g_ui.UpdateStateR(map(stroke,0,MAX_STROKEINMM,0,100));
          LogDebug(stroke);
          break;
          }
          break;

          case M_SET_PATTERN:
          if (buttonstate == BTN_LONG) {
          state = OPT_SET_PATTERN;
          g_ui.UpdateMessage("->Select Pattern<-");
          pattern = Stroker.getPattern();
          g_ui.UpdateMessage(Stroker.getPatternName(pattern));
          g_ui.UpdateTitelR("Pattern");
          break;
          }
          if (encoder->wasTurnedLeft()) {
          g_ui.UpdateMessage("Set Stroke");
          state = M_SET_STROKE;
          break;
          } else if (encoder->wasTurnedRight()) {
          g_ui.UpdateMessage("Inter. Depth");  
          state = M_SET_DEPTH_INT;
          break;
          }
          break;

          case OPT_SET_PATTERN:
          if (buttonstate == BTN_LONG) {
          state = M_SET_PATTERN;
          g_ui.UpdateMessage("Select Pattern");
          g_ui.UpdateStateR(zero);
          g_ui.UpdateTitelR("");
          Stroker.setPattern(pattern, false);
          break;
          }
          if (encoder->wasTurnedLeft()) {
          pattern = constrain((pattern - 1), 0, patternN);
          g_ui.UpdateMessage(Stroker.getPatternName(pattern));
          break;
          } else if (encoder->wasTurnedRight()) {
          pattern = constrain((pattern + 1), 0, patternN);
          g_ui.UpdateMessage(Stroker.getPatternName(pattern));
          break;
          }
          break;

          case M_SET_DEPTH_INT:
          if (buttonstate == BTN_LONG) {
          state = OPT_SET_DEPTH_INT;
          Stroker.setupDepth(10.0, false);
          depth = Stroker.getDepth();
          g_ui.UpdateTitelR("Depth");
          g_ui.UpdateStateR(map(depth,0,MAX_STROKEINMM,0,100));
          g_ui.UpdateMessage("->Inter. Depth<-");
          break;
          }
          if (encoder->wasTurnedLeft()) {
          g_ui.UpdateMessage("Set Pattern");
          state = M_SET_PATTERN;
          break;
          } else if (encoder->wasTurnedRight()) {
          g_ui.UpdateMessage("Depth Fancy");  
          state = M_SET_DEPTH_FANCY;
          break;
          }
          break;
          
          case OPT_SET_DEPTH_INT:
          if (buttonstate == BTN_LONG) {
          state = M_SET_DEPTH_INT;
          g_ui.UpdateMessage("Inter. Depth");
          g_ui.UpdateStateR(zero);
          g_ui.UpdateTitelR("");
          break;
          }
         if (encoder->wasTurnedLeft()) {
          depth = constrain((depth - DEPTH_RESULTION) , 0, MAX_STROKEINMM);
          Stroker.setDepth(depth, true);
          g_ui.UpdateStateR(map(depth,0,MAX_STROKEINMM,0,100));
          LogDebug(depth);
          break;
          } else if (encoder->wasTurnedRight()) {
          depth = constrain((depth + DEPTH_RESULTION) , 0, MAX_STROKEINMM);
          Stroker.setDepth(depth, true);
          g_ui.UpdateStateR(map(depth,0,MAX_STROKEINMM,0,100));
          LogDebug(depth);
          break;
          }
          break;

          case M_SET_DEPTH_FANCY:
          if (buttonstate == BTN_LONG) {
          state = OPT_SET_DEPTH_FANCY;
          g_ui.UpdateMessage("->Depth Fancy<-");
          Stroker.setupDepth(10.0, true);
          depth = Stroker.getDepth();
          g_ui.UpdateTitelR("Depth");
          g_ui.UpdateStateR(map(depth,0,MAX_STROKEINMM,0,100));
          break;
          }
          if (encoder->wasTurnedLeft()) {
          g_ui.UpdateMessage("Inter. Depth");
          state = M_SET_DEPTH_INT;
          break;
          } else if (encoder->wasTurnedRight()) {
          g_ui.UpdateMessage("Home");  
          state = M_MENUE;
          break;
          }
          break;

          case OPT_SET_DEPTH_FANCY:
          if (buttonstate == BTN_LONG) {
          state = M_SET_DEPTH_FANCY;
          g_ui.UpdateMessage("Depth Fancy");
          g_ui.UpdateStateR(zero);
          g_ui.UpdateTitelR("");
          break;
          }
          if (encoder->wasTurnedLeft()) {
          depth = constrain((depth - DEPTH_RESULTION) , 0, MAX_STROKEINMM);
          Stroker.setDepth(depth, true);
          g_ui.UpdateStateR(map(depth,0,MAX_STROKEINMM,0,100));
          LogDebug(depth);
          break;
          } else if (encoder->wasTurnedRight()) {
          depth = constrain((depth + DEPTH_RESULTION) , 0, MAX_STROKEINMM);
          Stroker.setDepth(depth, true);
          g_ui.UpdateStateR(map(depth,0,MAX_STROKEINMM,0,100));
          LogDebug(depth);
          break;
          }
          break;
    }
     speed = getAnalogAverage(SPEED_POT_PIN, 200); // get average analog reading, function takes pin and # samples
     g_ui.UpdateStateL(speed);
     //LogDebug(speed);
     speed = fscale(0.00, 99.98, 0.5, USER_SPEEDLIMIT, speed, -1);
     //LogDebug(speed);
     
     Stroker.setSpeed(speed, true);
     vTaskDelay(100);
   }
}

void espNowRemoteTask(void *pvParameters)
{
    WiFi.mode(WIFI_STA);
    LogDebug(WiFi.macAddress());

    // Init ESP-NOW
    if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
    }
    // Once ESPNow is successfully Init, we will register for Send CB to
    // get the status of Trasnmitted packet
    esp_now_register_send_cb(OnDataSent);

    // Register peer
    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = 0;  
    peerInfo.encrypt = false;
  
      // Add peer        
    if (esp_now_add_peer(&peerInfo) != ESP_OK){
    Serial.println("Failed to add peer");
    vTaskSuspend(eRemote_t);
    return;
    } else {
      vTaskSuspend(CRemote_T);
    }
    // Register for a callback function that will be called when data is received
    esp_now_register_recv_cb(OnDataRecv);

    for(;;)
    {
      vTaskDelay(500);
    }
}

float getAnalogAverage(int pinNumber, int samples)
{
    float sum = 0;
    float average = 0;
    float percentage = 0;
    for (int i = 0; i < samples; i++)
    {
        // TODO: Possibly use fancier filters?
        sum += analogRead(pinNumber);
    }
    average = sum / samples;
    // TODO: Might want to add a deadband
    
    percentage = 100.0 * average / 4096.0; // 12 bit resolution
    return percentage;
}

void setLedRainbow(CRGB leds[])
{
    // int power = 250;

    for (int hueShift = 0; hueShift < 350; hueShift++)
    {
        int gHue = hueShift % 255;
        fill_rainbow(leds, NUM_LEDS, gHue, 25);
        FastLED.show();
        delay(4);
    }
}

void almclick(){
  LogDebug("ALM clicked");
}

void pedclick(){
  LogDebug("PED clicked");
}