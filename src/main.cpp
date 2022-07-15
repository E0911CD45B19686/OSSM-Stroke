#include <Arduino.h>          // Basic Needs
#include <Wire.h>
#include <SPI.h>
#include <StrokeEngine.h>     // Include Stroke Engine
#include "OSSM_Config.h"      // START HERE FOR Configuration
#include "OSSM_PinDEF.h"      // This is where you set pins specific for your board
#include "FastLED.h"          // Used for the LED on the Reference Board (or any other pixel LEDS you may add)
#include "RotaryEncoder.h"
#include "OssmUi.h"           // Separate file that helps contain the OLED screen functions for Local Remotes

#define BTN_NONE   0
#define BTN_SHORT  1
#define BTN_LONG   2
#define BTN_V_LONG 3

volatile float speedPercentage = 0;
volatile float sensation = 0;

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
float zero = 0.0;
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

TaskHandle_t estop_T    = nullptr;  // Estop Taks for Emergency 
TaskHandle_t CRemote_T  = nullptr;  // Cable Remote Task 

#define BRIGHTNESS 170
#define LED_TYPE WS2811
#define COLOR_ORDER GRB
#define LED_PIN 25
#define NUM_LEDS 1
CRGB leds[NUM_LEDS];

// Declarations
void emergencyStopTask(void *pvParameters); // Handels all Higher Emergency Stop Functions
void CableRemoteTask(void *pvParameters);  // Handels all Functions from Cable Remote
void setLedRainbow(CRGB leds[]);

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

void setup() {
  Serial.begin(115200);         // Start Serial.
  LogDebug("\n Starting");      // Start LogDebug
  delay(200);
  
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(150);
  setLedRainbow(leds);
  FastLED.show();

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
  if(!g_ui.DisplayIsConnected()){
    vTaskSuspend(CRemote_T);
  }
  
  pinMode(SERVO_ALM_PIN, INPUT);
  pinMode(SERVO_PED_PIN, INPUT);

  pinMode(SPEED_POT_PIN, INPUT);
  adcAttachPin(SPEED_POT_PIN);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db); // allows us to read almost full 3.3V range
  
  // wait for homing to complete
  while (Stroker.getState() != READY) {
    delay(100);
  }
}

void loop() {
  g_ui.UpdateScreen();
}

void emergencyStopTask(void *pvParameters)
{
 for (;;)
  { 
    bool alm = digitalRead(SERVO_ALM_PIN);
    bool ped = digitalRead(SERVO_PED_PIN);
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
