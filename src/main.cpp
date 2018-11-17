/*===================================================================================================================*/
/* includes */
/*===================================================================================================================*/
#include <Arduino.h>

#include "config.h"
/* the config.h file contains your personal configuration of the parameters below: 
  #define WIFI_SSID                   "xxxxxxxxx"
  #define WIFI_PWD                    "xxx"
  #define LOCAL_MQTT_HOST             "123.456.789.012"
  #define THERMOSTAT_BINARY           "http://<domain or ip>/<name>.bin"
*/

#include "version.h"
#include <ESP8266WiFi.h>
#include <DHTesp.h>
#include "cThermostat.h"
#include <osapi.h>   /* for sensor timer */
#include <os_type.h> /* for sensor timer */
#include "cSystemState.h"
#include "UserFonts.h"
#include <SSD1306.h>
#include "MQTTClient.h"
#include "cMQTT.h"
#include <ESP8266httpUpdate.h>
#include <ArduinoOTA.h>
#include <FS.h> // SPIFFS

/*===================================================================================================================*/
/* GPIO config */
/*===================================================================================================================*/
#define DHT_PIN              2 /* sensor */
#define SDA_PIN              4 /* I²C */
#define SCL_PIN              5 /* I²C */
#define ENCODER_PIN1        12 /* rotary left/right */
#define ENCODER_PIN2        13 /* rotary left/right */
#define ENCODER_SWITCH_PIN  14 /* push button switch */
#define RELAY_PIN           16 /* relay control */

/*===================================================================================================================*/
/* variables, constants, types, classes, etc, definitions */
/*===================================================================================================================*/
/* WiFi */
const char*    ssid     = WIFI_SSID;
const char*    password = WIFI_PWD;
/* MQTT */
const char*    mqttHost                = LOCAL_MQTT_HOST;
const int      mqttPort                = LOCAL_MQTT_PORT;
unsigned long  mqttReconnectTime       = 0;
unsigned long  mqttReconnectInterval   = MQTT_RECONNECT_TIME;
/* HTTP Update */
const    String myUpdateServer = THERMOSTAT_BINARY;
bool     FETCH_UPDATE          = false;       /* global variable used to decide whether an update shall be fetched from server or not */
bool     HA_DISCOVER           = false;
/* OTA */
typedef enum { TH_OTA_IDLE, TH_OTA_ACTIVE, TH_OTA_FINISHED, TH_OTA_ERROR } OtaUpdate_t;       /* global variable used to change display in case OTA update is initiated */
OtaUpdate_t OTA_UPDATE = TH_OTA_IDLE;
/* rotary encoder */
unsigned long  switchDebounceTime       = 0;
unsigned long  switchDebounceInterval   = 250;
#define rotLeft              -1
#define rotRight              1
#define rotInit               0
#define tempStep              5
#define displayTemp           0
#define displayHumid          1
volatile int            LAST_ENCODED                    = 0b11;        /* initial state of the rotary encoders gray code */
volatile int            ROTARY_ENCODER_DIRECTION_INTS   = rotInit;     /* initialize rotary encoder with no direction */
/* senspr */
unsigned long readSensorScheduled = 0;
unsigned long readSensorScheduleTime = SENSOR_UPDATE_INTERVAL;       // 20 s in ms
/* Display */
#define drawTempYOffset       16
#define drawTargetTempYOffset 0
#define drawHeating drawXbm(0,drawTempYOffset,myThermo_width,myThermo_height,myThermo)
/* classes */
DHTesp         myDHT;
SSD1306        myDisplay(0x3c,SDA_PIN,SCL_PIN);
WiFiClient     myWiFiClient;
Thermostat     myThermostat;
SystemState    mySystemState;
MQTTClient     myMqttClient(1000);
mqttHelper     myMqttHelper;

#define        SPIFFS_MQTT_ID_FILE        String("/itsme")
#define        SPIFFS_SENSOR_CALIB_FILE   String("/sensor")
#define        SPIFFS_TARGET_TEMP_FILE    String("/targetTemp")
#define        SPIFFS_WRITE_DEBOUNCE      20000 /* write target temperature to spiffs if it wasn't changed for 20 s (time in ms) */
boolean        SPIFFS_WRITTEN =           true;
unsigned long  SPIFFS_REFERENCE_TIME;
int            SPIFFS_LAST_TARGET_TEMPERATURE;

/*===================================================================================================================*/
/* function declarations */
/*===================================================================================================================*/
void GPIO_CONFIG(void);
void SPIFFS_INIT(void);
void SPIFFS_MAIN(void);
void DISPLAY_INIT(void);
void WIFI_CONNECT(void);
void HANDLE_SYSTEM_STATE(void);
void SENSOR_MAIN(void);
LOCAL void sensor_cb(void *arg); /* sensor timer callback */
void HEATING_CONTROL_MAIN(void);
void DRAW_DISPLAY_MAIN(void);
void MQTT_CONNECT(void);
void MQTT_MAIN(void);
void messageReceived(String &topic, String &payload); /* MQTT callback */
void OTA_INIT(void);
void HANDLE_OTA_UPDATE(void);
void HANDLE_HTTP_UPDATE(void);
void encoderSwitch (void);
void updateEncoder(void);
void homeAssistantDiscovery(void);

/*===================================================================================================================*/
/* library functions */
/*===================================================================================================================*/
float  intToFloat(int intValue)           { return ((float)(intValue/10.0)); }
int    floatToInt(float floatValue)       { return ((int)(floatValue * 10)); }
String boolToStringOnOff(bool boolean)    { return (boolean == true ? "on" : "off"); }
String boolToStringHeatOff(bool boolean)  { return (boolean == true ? "heat" : "off"); }

/* Thanks to Tasmota  for timer functions */
long TimeDifference(unsigned long prev, unsigned long next)
{
  // Return the time difference as a signed value, taking into account the timers may overflow.
  // Returned timediff is between -24.9 days and +24.9 days.
  // Returned value is positive when "next" is after "prev"
  long signed_diff = 0;
  // To cast a value to a signed long, the difference may not exceed half 0xffffffffUL (= 4294967294)
  const unsigned long half_max_unsigned_long = 2147483647u;  // = 2^31 -1
  if (next >= prev) {
    const unsigned long diff = next - prev;
    if (diff <= half_max_unsigned_long) {                    // Normal situation, just return the difference.
      signed_diff = static_cast<long>(diff);                 // Difference is a positive value.
    } else {
      // prev has overflow, return a negative difference value
      signed_diff = static_cast<long>((0xffffffffUL - next) + prev + 1u);
      signed_diff = -1 * signed_diff;
    }
  } else {
    // next < prev
    const unsigned long diff = prev - next;
    if (diff <= half_max_unsigned_long) {                    // Normal situation, return a negative difference value
      signed_diff = static_cast<long>(diff);
      signed_diff = -1 * signed_diff;
    } else {
      // next has overflow, return a positive difference value
      signed_diff = static_cast<long>((0xffffffffUL - prev) + next + 1u);
    }
  }
  return signed_diff;
}

long TimePassedSince(unsigned long timestamp)
{
  // Compute the number of milliSeconds passed since timestamp given.
  // Note: value can be negative if the timestamp has not yet been reached.
  return TimeDifference(timestamp, millis());
}

bool TimeReached(unsigned long timer)
{
  // Check if a certain timeout has been reached.
  const long passed = TimePassedSince(timer);
  return (passed >= 0);
}

void SetNextTimeInterval(unsigned long& timer, const unsigned long step)
{
  timer += step;
  const long passed = TimePassedSince(timer);
  if (passed < 0) { return; }   // Event has not yet happened, which is fine.
  if (static_cast<unsigned long>(passed) > step) {
    // No need to keep running behind, start again.
    timer = millis() + step;
    return;
  }
  // Try to get in sync again.
  timer = millis() + (step - passed);
}

String readSpiffs(String file)
{
   String fileContent = "";

   if (SPIFFS.exists(file))
   {
      File f = SPIFFS.open(file, "r");
      if (!f) {
         Serial.println("oh no, failed to open file: "+ file);
         /* TODO: Error handling */
      }
      else
      {
         fileContent = f.readStringUntil('\n');
         f.close();

         if (fileContent == "")
         {
            Serial.println("File " + file + " is empty");
         }
      }
   }
   else
   {
      Serial.println("File " + file + " does not exist");
   }
   return fileContent;
}

boolean writeSpiffs(String file, String newFileContent)
{
   boolean success = false; /* return success = true if no error occured and the newFileContent equals the file content */
   String  currentFileContent = "";

   if (SPIFFS.exists(file))
   {
      #ifdef CFG_DEBUG
      File f = SPIFFS.open(file, "r");
      currentFileContent = f.readStringUntil('\n');
      f.close();
      #endif
   }

   if (currentFileContent == newFileContent)
   {
      success = true; /* nothing to do, consider write as successful */

      #ifdef CFG_DEBUG
      Serial.println("Content of '" + file + "' before writing: " + currentFileContent + " equals new content: " + newFileContent + ". Nothing to do");
      #endif
   }
   else
   {
      #ifdef CFG_DEBUG
      Serial.println("Content of '" + file + "' before writing: " + currentFileContent);
      #endif

      SPIFFS.remove(file);

      File f = SPIFFS.open(file, "w");
      if (!f)
      {
         #ifdef CFG_DEBUG
         Serial.println("Failed to open file: " + file + " for writing");
         #endif /* CFG_DEBUG */

         /* TODO: Error handling */
      }
      else
      {
         #ifdef CFG_DEBUG
         Serial.println("New content of '" + file + "' is: " + newFileContent);
         #endif /* CFG_DEBUG */

         f.print(newFileContent);
         f.close();
         success = true; /* new content written */
         delay(1000);
      }
   }
   return success;
}

bool splitSensorDataString(String sensorCalib, int *offset, int *factor)
{
   bool ret = false;
   String delimiter = ";";
   size_t pos = 0;

   pos = (sensorCalib.indexOf(delimiter));

   /* don't accept empty substring or strings without delimiter */
   if ((pos > 0) && (pos < UINT32_MAX))
   {
      ret = true;
      *offset = (sensorCalib.substring(0, pos)).toInt();
      *factor = (sensorCalib.substring(pos+1)).toInt();
   }
   #ifdef CFG_DEBUG
   else
   {
      Serial.println("Malformed sensor calibration string >> calibration update denied");
   }

   Serial.println("Delimiter: " + delimiter);
   Serial.println("Position of delimiter: " + String(pos));
   Serial.println("Offset: " + String(*offset));
   Serial.println("Factor: " + String(*factor));
   #endif
   return ret;
}

/*===================================================================================================================*/
/* The setup function is called once at startup of the sketch */
/*===================================================================================================================*/
void setup()
{
   Serial.begin(115200);
   delay(1000);
   #ifdef CFG_DEBUG
   Serial.println("Serial connection established");
   #endif

   SPIFFS_INIT();   /* read stuff from SPIFFS */
   GPIO_CONFIG();    /* configure GPIOs */
   myDHT.setup(DHT_PIN, DHTesp::DHT22);    /* init DHT sensor */
   DISPLAY_INIT();   /* init Display */
   WIFI_CONNECT();   /* connect to WiFi */
   OTA_INIT();
   myMqttHelper.setup();
   MQTT_CONNECT();   /* connect to MQTT host and build subscriptions, must be called after SPIFFS_INIT()*/

   /* enable interrupts on encoder pins to decode gray code and recognize switch event*/
   attachInterrupt(ENCODER_PIN1, updateEncoder, CHANGE);
   attachInterrupt(ENCODER_PIN2, updateEncoder, CHANGE);
   attachInterrupt(ENCODER_SWITCH_PIN, encoderSwitch, FALLING);

}

/*===================================================================================================================*/
/* functions called by setup() */
/*===================================================================================================================*/
void SPIFFS_INIT(void)
{
   SPIFFS.begin();

   if (!SPIFFS.exists("/formatted"))
   {
      /* This code is only run once to format the SPIFFS before first usage */
      #ifdef CFG_DEBUG
      Serial.println("Formatting SPIFFS, this takes some time");
      #endif
      SPIFFS.format();
      #ifdef CFG_DEBUG
      Serial.println("Formatting SPIFFS finished");
      Serial.println("Open file '/formatted' in write mode");
      #endif
      File f = SPIFFS.open("/formatted", "w");
      if (!f) {
          Serial.println("file open failed");
      }
      else
      {
         f.close();
         delay(5000);
         if (!SPIFFS.exists("/formatted"))
         {
            Serial.println("That didn't work!");
         }
         else
         {
            #ifdef CFG_DEBUG
            Serial.println("Cool, working!");
            #endif
         }
      }
   }
   else
   {
      #ifdef CFG_DEBUG
      Serial.println("Found '/formatted' >> SPIFFS ready to use");
      #endif
   }
   #ifdef CFG_DEBUG
   Serial.println("Check if I remember who I am ... ");
   #endif


   #ifdef CFG_DEBUG
   Dir dir = SPIFFS.openDir("/");
   while (dir.next()) {
       Serial.print("SPIFFS file found: " + dir.fileName() + " - Size in byte: ");
       File f = dir.openFile("r");
       Serial.println(f.size());
   }
   #endif

   /* check name for MQTT */
   String myName = readSpiffs(SPIFFS_MQTT_ID_FILE);

   if (myName != "")
   {
      myMqttHelper.setName(myName); /* set name from spiffs here, thus setup only needs to set the main topic */
   }
   else
   {
      Serial.println("File " + SPIFFS_MQTT_ID_FILE + " is empty or does not exist, proceed as 'unknown'");
   }
   Serial.println("My name is: " + myMqttHelper.getName());

   String sensorCalib = readSpiffs(SPIFFS_SENSOR_CALIB_FILE);

   /* check parameters for Sensor calibration */
   if (sensorCalib != "")
   {
      int offset;
      int factor;

      /* split parameter string */
      if (splitSensorDataString(sensorCalib, &offset, &factor))
      {
         #ifdef CFG_DEBUG
         Serial.println("Offset read from /sensor is: " + String(offset) + " *C");
         Serial.println("Factor read from /sensor is: " + String(factor) + " %");
         #endif

         myThermostat.setSensorCalibData(offset, factor, false);
      }
      else
      {
         Serial.println("File " + SPIFFS_SENSOR_CALIB_FILE + " returned malformed string, proceed with uncalibrated sensor");
      }
   }
   else
   {
      Serial.println("File " + SPIFFS_SENSOR_CALIB_FILE + " is empty or does not exist, proceed with uncalibrated sensor");
   }

   String targetTemp = readSpiffs(SPIFFS_TARGET_TEMP_FILE);
   if (targetTemp != "")
   {
      myThermostat.setTargetTemperature(targetTemp.toInt()); /* set temperature from spiffs here*/
      Serial.println("File " + SPIFFS_TARGET_TEMP_FILE + " is available, proceed with value: " + String(myThermostat.getTargetTemperature()));
   }
   else
   {
      Serial.println("File " + SPIFFS_TARGET_TEMP_FILE + " is empty or does not exist, proceed with init value: " + String(myThermostat.getTargetTemperature()));
   }

   SPIFFS_LAST_TARGET_TEMPERATURE = myThermostat.getTargetTemperature(); /* store this value also here to avoid unnecessary usage of SPIFFS */
}

void GPIO_CONFIG(void)
{
   myThermostat.setup(RELAY_PIN, myThermostat.getTargetTemperature()); /*GPIO to switch connected relay and initial target temperature */

   /* initialize encoder pins */
   pinMode(ENCODER_PIN1, INPUT_PULLUP);
   pinMode(ENCODER_PIN2, INPUT_PULLUP);
   pinMode(ENCODER_SWITCH_PIN, INPUT_PULLUP);
}

/* Display */
void DISPLAY_INIT(void)
{
   #ifdef CFG_DEBUG
   Serial.println("Initialize display");
   #endif
   Wire.begin();   /* needed for I²C communication with display */
   myDisplay.init();
   myDisplay.flipScreenVertically();
   myDisplay.clear();
   myDisplay.setFont(Roboto_Condensed_16);
   myDisplay.setTextAlignment(TEXT_ALIGN_CENTER);
   myDisplay.drawString(64, 11, "Initialize");
   myDisplay.drawString(64, 33, myMqttHelper.getName());
   myDisplay.display();
}

void WIFI_CONNECT(void)
{
   /* init WiFi */
   if (WiFi.status() != WL_CONNECTED)
   {
      #ifdef CFG_DEBUG
      Serial.println("Initialize WiFi ");
      #endif

      WiFi.mode(WIFI_STA);
      WiFi.begin(ssid, password);

      #ifdef CFG_DEBUG
      Serial.println("WiFi Status: "+ String(WiFi.begin(ssid, password)));
      #endif
   }

   /* try to connect to WiFi, proceed offline if not connecting here*/
   while (WiFi.waitForConnectResult() != WL_CONNECTED) {
     Serial.println("Failed to connect to WiFi, continue offline");
     mySystemState.setSystemState(systemState_offline);
     return;
   }

   mySystemState.setSystemState(systemState_online);
   Serial.print("IP address: ");
   Serial.println(WiFi.localIP());

}

/* OTA */
void OTA_INIT(void)
{
   if (systemState_online == mySystemState.getSystemState())
   {
      /* convert string to char* */
      char *hostname = new char[myMqttHelper.getLoweredName().length() + 1];
      strcpy(hostname,myMqttHelper.getLoweredName().c_str());

      ArduinoOTA.setHostname(hostname);

      delete [] hostname;

      ArduinoOTA.onStart([]()
      {
         OTA_UPDATE = TH_OTA_ACTIVE;
         DRAW_DISPLAY_MAIN();
         Serial.println("Start");
      });

      ArduinoOTA.onEnd([]()
      {
         OTA_UPDATE = TH_OTA_FINISHED;
         DRAW_DISPLAY_MAIN();
         Serial.println("\nEnd");
      });

      ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
      {
         Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
      });

      ArduinoOTA.onError([](ota_error_t error)
      {
         OTA_UPDATE = TH_OTA_ERROR;
         DRAW_DISPLAY_MAIN();

         Serial.printf("Error[%u]: ", error);
         if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
         else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
         else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
         else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
         else if (error == OTA_END_ERROR) Serial.println("End Failed");
      });
      ArduinoOTA.begin();
   }
}

void MQTT_CONNECT(void)
{
   if (systemState_online == mySystemState.getSystemState())
   {
      boolean ret;
      /* convert name string to char for connect API */
      char *mqttClientName = new char[myMqttHelper.getLoweredName().length() + 1];
      strcpy(mqttClientName,myMqttHelper.getLoweredName().c_str());

      /* convert name string to char for setWill API */
      char *mqttWillTopic = new char[myMqttHelper.getTopicLastWill().length() + 1];
      strcpy(mqttWillTopic, myMqttHelper.getTopicLastWill().c_str());

      /* broker shall publish 'offline' on ungraceful disconnect >> Last Will */
      myMqttClient.setWill(mqttWillTopic ,"offline", false, 1);
      myMqttClient.begin(mqttHost, mqttPort, myWiFiClient);
      myMqttClient.onMessage(messageReceived);       /* register callback */
      ret = myMqttClient.connect(mqttClientName, LOCAL_MQTT_USER, LOCAL_MQTT_PWD);

      /* subscribe some topics */
      ret = myMqttClient.subscribe(myMqttHelper.getTopicTargetTempCmd());
      ret = myMqttClient.subscribe(myMqttHelper.getTopicThermostatModeCmd());
      ret = myMqttClient.subscribe(myMqttHelper.getTopicUpdateFirmware());
      ret = myMqttClient.subscribe(myMqttHelper.getTopicChangeName());
      ret = myMqttClient.subscribe(myMqttHelper.getTopicChangeSensorCalib());
      ret = myMqttClient.subscribe(myMqttHelper.getTopicSystemRestartRequest());
      myMqttClient.loop();

      homeAssistantDiscovery();

      /* publish restart = false on connect */
      myMqttClient.publish(myMqttHelper.getTopicSystemRestartRequest(),       "0",                                            false, 1);
      /* publish online in will topic */
      myMqttClient.publish(myMqttHelper.getTopicLastWill(),                   "online",                                       false, 1);

      delete [] mqttWillTopic;
      delete [] mqttClientName;
   }
}

/*===================================================================================================================*/
/* The loop function is called in an endless loop */
/*===================================================================================================================*/
void loop()
{
   HANDLE_SYSTEM_STATE(); /*handle system state class and trigger reconnects */
   SENSOR_MAIN();   /* get sensor data */
   HEATING_CONTROL_MAIN(); /* control relay for heating */
   MQTT_MAIN(); /* handle MQTT each loop */
   DRAW_DISPLAY_MAIN(); /* draw display each loop */
   HANDLE_HTTP_UPDATE(); /* pull update from server if it was requested via MQTT*/
   HANDLE_OTA_UPDATE();
   SPIFFS_MAIN();

   yield();
}

/*===================================================================================================================*/
/* functions called by loop() */
/*===================================================================================================================*/
void HANDLE_SYSTEM_STATE(void)
{
   if (mySystemState.getSystemRestartRequest() == true)
   {
      DRAW_DISPLAY_MAIN();

      Serial.println("Restarting in 3 seconds");
      delay(3000);
      ESP.restart();
   }

   if (WiFi.status() == WL_CONNECTED)
   {
      if (mySystemState.getSystemState() == systemState_offline)
      {
         mySystemState.setSystemState(systemState_online);
      }
   }
   else
   {
      if (mySystemState.getSystemState() == systemState_online)
      {
         mySystemState.setSystemState(systemState_offline);
      }
   }

   if (systemState_offline == mySystemState.getSystemState())
   {
      if (mySystemState.getConnectDebounceTimer() == 0) /* getConnectDebounceTimer decrements the timer */
      {
         /* try to come online, debounced to avoid reconnect each loop*/
         WIFI_CONNECT();
         MQTT_CONNECT();
         OTA_INIT();
      }
   }
}

void SENSOR_MAIN()
{
   float dhtTemp;
   float dhtHumid;

   /* schedule routine for sensor read */
   if (TimeReached(readSensorScheduled))
   {
      SetNextTimeInterval(readSensorScheduled, readSensorScheduleTime);

      /* Reading temperature or humidity takes about 250 milliseconds! */
      /* Sensor readings may also be up to 2 seconds 'old' (its a very slow sensor) */
      dhtHumid = myDHT.getHumidity();
      dhtTemp  = myDHT.getTemperature();

      /* Check if any reads failed */
      if (isnan(dhtHumid) || isnan(dhtTemp)) {
         myThermostat.setLastSensorReadFailed(true);   /* set failure flag and exit SENSOR_MAIN() */
         #ifdef CFG_DEBUG
         Serial.println("Failed to read from DHT sensor! Failure counter: " + String(myThermostat.getSensorFailureCounter()));
         #endif
      }
      else
      {
         myThermostat.setLastSensorReadFailed(false);   /* set no failure during read sensor */

         myThermostat.setCurrentHumidity((int)(10* dhtHumid));     /* read value and convert to one decimal precision integer */
         myThermostat.setCurrentTemperature((int)(10* dhtTemp));   /* read value and convert to one decimal precision integer */

         #ifdef CFG_DEBUG
         Serial.print("Temperature: ");
         Serial.print(intToFloat(myThermostat.getCurrentTemperature()),1);
         Serial.println(" *C ");
         Serial.print("Filtered temperature: ");
         Serial.print(intToFloat(myThermostat.getFilteredTemperature()),1);
         Serial.println(" *C ");

         Serial.print("Humidity: ");
         Serial.print(intToFloat(myThermostat.getCurrentHumidity()),1);
         Serial.println(" %");
         Serial.print("Filtered humidity: ");
         Serial.print(intToFloat(myThermostat.getFilteredHumidity()),1);
         Serial.println(" %");
         #endif

         #ifdef CFG_PRINT_TEMPERATURE_QUEUE
         for (int i = 0; i < CFG_MEDIAN_QUEUE_SIZE; i++)
         {
            int temperature;
            int humidity;
            if ( (myTemperatureFilter.getRawSampleValue(i,&temperature)) && (myHumidityFilter.getRawSampleValue(i,&humidity)))
            {
               Serial.print("Sample ");
               Serial.print(i);
               Serial.print(": ");
               Serial.print(intToFloat(temperature),1);
               Serial.print(", ");
               Serial.println(intToFloat(humidity),1);
            }
         }
         #endif
      }
   }
}

void HEATING_CONTROL_MAIN(void)
{
   if (myThermostat.getSensorError())
   {
      /* switch off heating if sensor does not provide values */
      #ifdef CFG_DEBUG
      Serial.println("not heating, sensor data invalid");
      #endif
      if (myThermostat.getActualState() == TH_HEAT)
      {
         myThermostat.setActualState(TH_OFF);
      }
   }
   else /* sensor is healthy */
   {
      if (myThermostat.getThermostatMode() == TH_HEAT) /* check if heating is allowed by user */
      {
         if (myThermostat.getFilteredTemperature() < myThermostat.getTargetTemperature()) /* check if measured temperature is lower than heating target */
         {
            if (myThermostat.getActualState() == TH_OFF) /* switch on heating if target temperature is higher than measured temperature */
            {
               #ifdef CFG_DEBUG
               Serial.println("heating");
               #endif
               myThermostat.setActualState(TH_HEAT);
            }
         }
         else if (myThermostat.getFilteredTemperature() > myThermostat.getTargetTemperature()) /* check if measured temperature is higher than heating target */
         {
            if (myThermostat.getActualState() == TH_HEAT) /* switch off heating if target temperature is lower than measured temperature */
            {
               #ifdef CFG_DEBUG
               Serial.println("not heating");
               #endif
               myThermostat.setActualState(TH_OFF);
            }
         }
         else
         {
            /* remain in current heating state if temperatures are equal */
         }
      }
      else
      {
         /* disable heating if heating is set to not allowed by user */
         myThermostat.setActualState(TH_OFF);
      }
   }
}

void DRAW_DISPLAY_MAIN(void)
{
   myDisplay.clear();

   myDisplay.setContrast(50);

   if (mySystemState.getSystemRestartRequest() == true)
   {
      myDisplay.setFont(Roboto_Condensed_16);
      myDisplay.setTextAlignment(TEXT_ALIGN_CENTER);
      myDisplay.drawString(64,22,"Restart");
   }
   /* OTA */
   else if (OTA_UPDATE != TH_OTA_IDLE)
   {
      myDisplay.setFont(Roboto_Condensed_16);
      myDisplay.setTextAlignment(TEXT_ALIGN_CENTER);
      switch (OTA_UPDATE) {
         case TH_OTA_ACTIVE:
            myDisplay.drawString(64,22,"Update");
            break;
         case TH_OTA_FINISHED:
            myDisplay.drawString(64,22,"Finished");
            break;
         case TH_OTA_ERROR:
            myDisplay.drawString(64,22,"Error");
            break;
      }
   }
   /* HTTP Update */
   else if (FETCH_UPDATE == true)
   {
      myDisplay.setFont(Roboto_Condensed_16);
      myDisplay.setTextAlignment(TEXT_ALIGN_CENTER);
      myDisplay.drawString(64,22,"Update");
   }
   else
   {
      myDisplay.setTextAlignment(TEXT_ALIGN_RIGHT);
      myDisplay.setFont(Roboto_Condensed_32);

      if (myThermostat.getSensorError())
      {
         myDisplay.drawString(128, drawTempYOffset, "err");
      }
      else
      {
         myDisplay.drawString(128, drawTempYOffset, String(intToFloat(myThermostat.getFilteredTemperature()),1));
      }

      /* do not display target temperature if heating is not allowed */
      if (myThermostat.getThermostatMode() == TH_HEAT)
      {
         myDisplay.setFont(Roboto_Condensed_16);
         myDisplay.drawString(128, drawTargetTempYOffset, String(intToFloat(myThermostat.getTargetTemperature()),1));

         if (myThermostat.getActualState()) /* heating */
         {
            myDisplay.drawHeating;
         }
      }
   }
   #ifdef CFG_DEBUG
   myDisplay.setTextAlignment(TEXT_ALIGN_LEFT);
   myDisplay.setFont(Roboto_Condensed_16);
   myDisplay.drawString(0, drawTargetTempYOffset, String(VERSION));
   #endif

   myDisplay.display();
}

void MQTT_MAIN(void)
{
   if (systemState_online == mySystemState.getSystemState())
   {
      if(!myMqttClient.connected())
      {
         if (TimeReached(mqttReconnectTime))
         {
            MQTT_CONNECT();
            SetNextTimeInterval(mqttReconnectTime, mqttReconnectInterval); /* reset interval if MQTT_CONNECT was called*/
         }
         else  /* try reconnect to MQTT broker after mqttReconnectTime expired */
         {
           /* just wait */
         }
         return; /* no MQTT connection */
      }
      else
      {
         SetNextTimeInterval(mqttReconnectTime, mqttReconnectInterval); /* reset interval  if everything is fine */
      }

      /* HTTP Update */
      if (FETCH_UPDATE == true)
      {
         myMqttClient.publish(myMqttHelper.getTopicUpdateFirmwareAccepted(), String(false), false, 1); /* publish accepted update with value false to reset the switch in Home Assistant */
      }

      if (HA_DISCOVER == true)
      {
         homeAssistantDiscovery();
         HA_DISCOVER = false;
      }

      /* check if there is new data to transmit */
      if (myThermostat.getNewData())
      {
         myThermostat.resetNewData();
         myMqttClient.publish(myMqttHelper.getTopicData(), myMqttHelper.buildStateJSON(String(intToFloat(myThermostat.getFilteredTemperature())), String(intToFloat(myThermostat.getFilteredHumidity())), String(boolToStringOnOff(myThermostat.getActualState())), String(intToFloat(myThermostat.getTargetTemperature())), String(myThermostat.getSensorError()), String(boolToStringHeatOff(myThermostat.getThermostatMode())), String(myThermostat.getSensorCalibFactor()), String(intToFloat(myThermostat.getSensorCalibOffset())), WiFi.localIP().toString(), String(FIRMWARE_VERSION)), false, 1);
         myMqttClient.publish(myMqttHelper.getTopicLastWill(),"online", false, 1);
      }

      myMqttClient.loop();
      delay(20); // <- fixes some issues with WiFi stability
   }
}

void HANDLE_HTTP_UPDATE(void)
{
   if (FETCH_UPDATE == true)
   {
      DRAW_DISPLAY_MAIN();
      FETCH_UPDATE = false;
      Serial.printf("Remote update started");

      t_httpUpdate_return ret = ESPhttpUpdate.update(myUpdateServer);

      switch(ret) {
          case HTTP_UPDATE_FAILED:
              Serial.printf("HTTP_UPDATE_FAILD Error (%d): %s", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
              break;

          case HTTP_UPDATE_NO_UPDATES:
             Serial.println("HTTP_UPDATE_NO_UPDATES");
              break;

          case HTTP_UPDATE_OK:
             Serial.println("HTTP_UPDATE_OK");
              break;
      }
   }
}

void HANDLE_OTA_UPDATE(void)
{
   if (systemState_online == mySystemState.getSystemState())
   {
      ArduinoOTA.handle();
   }
}

void SPIFFS_MAIN(void)
{
   if (myMqttHelper.getNameChanged())
   {
      mySystemState.setSystemRestartRequest(true);

      if (!writeSpiffs(SPIFFS_MQTT_ID_FILE, myMqttHelper.getName()))
      {
         /* ToDo: implement retry */
      }
   }

   if(myThermostat.getNewCalib())
   {
      mySystemState.setSystemRestartRequest(true);
   }

   /* avoid extensive writing to SPIFFS, therefore check if the target temperature didn't change for a certain time before writing. */
   if (myThermostat.getTargetTemperature() != SPIFFS_LAST_TARGET_TEMPERATURE)
   {
      SPIFFS_REFERENCE_TIME = millis();
      SPIFFS_LAST_TARGET_TEMPERATURE = myThermostat.getTargetTemperature();
      SPIFFS_WRITTEN = false;
   }
   else /* target temperature not changed this loop */
   {
      if (SPIFFS_WRITTEN == true)
      {
         /* do nothing, last change was already stored in SPIFFS */
      }
      else /* latest change not stored in SPIFFS */
      {
         if (SPIFFS_REFERENCE_TIME + SPIFFS_WRITE_DEBOUNCE < millis()) /* check debounce */
         {
            /* debounce expired -> write */
            if (writeSpiffs(SPIFFS_TARGET_TEMP_FILE, String(myThermostat.getTargetTemperature())))
            {
               SPIFFS_WRITTEN = true;
            }
            else
            {
               /* SPIFFS not written, retry next loop */
            }
         }
         else
         {
            /* debounce SPIFFS write */
         }
      }
   }
}
/*===================================================================================================================*/
/* callback, interrupt, timer, other functions */
/*===================================================================================================================*/

void ICACHE_RAM_ATTR encoderSwitch(void)
{
   /* debouncing routine for encoder switch */
   if (TimeReached(switchDebounceTime))
   {
      SetNextTimeInterval(switchDebounceTime, switchDebounceInterval);
      myThermostat.toggleThermostatMode();
   }
}

void ICACHE_RAM_ATTR updateEncoder(void)
{
   int MSB = digitalRead(ENCODER_PIN1);
   int LSB = digitalRead(ENCODER_PIN2);

   int encoded = (MSB << 1) |LSB;  /* converting the 2 pin value to single number */

   int sum  = (LAST_ENCODED << 2) | encoded; /* adding it to the previous encoded value */

   if(sum == 0b1101 || sum == 0b0100 || sum == 0b0010 || sum == 0b1011) /* gray patterns for rotation to right */
   {
      ROTARY_ENCODER_DIRECTION_INTS += rotRight; /* count rotation interrupts to left and right, this shall make the encoder routine more robust against bouncing */
   }
   if(sum == 0b1110 || sum == 0b0111 || sum == 0b0001 || sum == 0b1000)  /* gray patterns for rotation to left */
   {
      ROTARY_ENCODER_DIRECTION_INTS += rotLeft;
   }

   if (encoded == 0b11) /* if the encoder is in an end position, evaluate the number of interrupts to left/right. simple majority wins */
   {
      #ifdef CFG_DEBUG
      Serial.println("Rotary encoder interrupts: " + String(ROTARY_ENCODER_DIRECTION_INTS));
      #endif

      if(ROTARY_ENCODER_DIRECTION_INTS > rotRight) /* if there was a higher amount of interrupts to the right, consider the encoder was turned to the right */
      {
          myThermostat.increaseTargetTemperature(tempStep);
       }
      else if (ROTARY_ENCODER_DIRECTION_INTS < rotLeft) /* if there was a higher amount of interrupts to the left, consider the encoder was turned to the left */
      {
          myThermostat.decreaseTargetTemperature(tempStep);
       }
      else
      {
         /* do nothing here, left/right interrupts have occurred with same amount -> should never happen */
      }
      ROTARY_ENCODER_DIRECTION_INTS = rotInit; /* reset interrupt count for next sequence */
   }

   LAST_ENCODED = encoded; /* store this value for next time */
}

void homeAssistantDiscovery(void)
{
   /* Home Assistant discovery on connect */
   myMqttClient.publish(myMqttHelper.getTopicHassDiscoveryClimate(),       myMqttHelper.buildHassDiscoveryClimate(),       false, 1);    // make HA discover the climate component
   myMqttClient.loop();
   myMqttClient.publish(myMqttHelper.getTopicHassDiscoveryBinarySensor(),  myMqttHelper.buildHassDiscoveryBinarySensor(),  false, 1);    // make HA discover the binary_sensor for sensor failure
   myMqttClient.loop();
   myMqttClient.publish(myMqttHelper.getTopicHassDiscoverySensor(sTemp),   myMqttHelper.buildHassDiscoverySensor(sTemp),   false, 1);    // make HA discover the temperature sensor
   myMqttClient.loop();
   myMqttClient.publish(myMqttHelper.getTopicHassDiscoverySensor(sHum),    myMqttHelper.buildHassDiscoverySensor(sHum),    false, 1);    // make HA discover the humidity sensor
   myMqttClient.loop();
   myMqttClient.publish(myMqttHelper.getTopicHassDiscoverySensor(sIP),     myMqttHelper.buildHassDiscoverySensor(sIP),     false, 1);    // make HA discover the IP sensor
   myMqttClient.loop();
   myMqttClient.publish(myMqttHelper.getTopicHassDiscoverySensor(sCalibF), myMqttHelper.buildHassDiscoverySensor(sCalibF), false, 1);    // make HA discover the sensor scaling sensor
   myMqttClient.loop();
   myMqttClient.publish(myMqttHelper.getTopicHassDiscoverySensor(sCalibO), myMqttHelper.buildHassDiscoverySensor(sCalibO), false, 1);    // make HA discover the sensor offset sensor
   myMqttClient.loop();
   myMqttClient.publish(myMqttHelper.getTopicHassDiscoverySensor(sFW),     myMqttHelper.buildHassDiscoverySensor(sFW),     false, 1);    // make HA discover the firmware version sensor
   myMqttClient.loop();
   myMqttClient.publish(myMqttHelper.getTopicHassDiscoverySwitch(swReset), myMqttHelper.buildHassDiscoverySwitch(swReset), false, 1);    // make HA discover the reset switch
   myMqttClient.loop();
   myMqttClient.publish(myMqttHelper.getTopicHassDiscoverySwitch(swUpdate),myMqttHelper.buildHassDiscoverySwitch(swUpdate),false, 1);    // make HA discover the update switch
   myMqttClient.loop();
}

void messageReceived(String &topic, String &payload)
{
   #ifdef CFG_DEBUG
   Serial.println("received: " + topic + " : " + payload);
   #endif

   if (topic == myMqttHelper.getTopicSystemRestartRequest())
   {
      #ifdef CFG_DEBUG
      Serial.println("Restart request received with value: " + payload);
      #endif
      if (payload == "1")
      {
         mySystemState.setSystemRestartRequest(true);
      }
      else
      {
         mySystemState.setSystemRestartRequest(false);
      }

      
   }

   /* HTTP Update */
   else if (topic == myMqttHelper.getTopicUpdateFirmware())
   {
      if (payload == "1")
      {
         #ifdef CFG_DEBUG
         Serial.println("Firmware updated triggered via MQTT");
         #endif
         FETCH_UPDATE = true;
      }
      else
      {
         FETCH_UPDATE = false;
      }
   }

   /* check incoming target temperature, don't set same target temperature as new*/
   else if (topic == myMqttHelper.getTopicTargetTempCmd())
   {
      if (myThermostat.getTargetTemperature() != payload.toFloat())
      {
         /* set new target temperature for heating control, min/max limitation inside (string to float to int) */
         myThermostat.setTargetTemperature(floatToInt(payload.toFloat()));
      }
   }


   else if (topic == myMqttHelper.getTopicThermostatModeCmd())
   {
      if (String("heat") == payload)
      {
         myThermostat.setThermostatMode(TH_HEAT);
      }
      else if (String("off") == payload)
      {
         myThermostat.setThermostatMode(TH_OFF);
      }
      else
      {
          /* do nothing */
      }
   }


   else if (topic == myMqttHelper.getTopicChangeName())
   {
      #ifdef CFG_DEBUG
      Serial.println("New name received: " + payload);
      #endif
      if (payload != myMqttHelper.getName())
      {
         #ifdef CFG_DEBUG
         Serial.println("Old name was: " + myMqttHelper.getName());
         #endif
         myMqttHelper.changeName(payload);
      }
   }

   else if(topic == myMqttHelper.getTopicChangeSensorCalib())
   {
      #ifdef CFG_DEBUG
      Serial.println("New sensor calibration parameters received: " + payload);
      #endif

      int offset;
      int factor;

      if (splitSensorDataString(payload, &offset, &factor))
      {
         myThermostat.setSensorCalibData(offset, factor, true);
      }
   }
   else if (topic == myMqttHelper.getTopicMqttDiscoveryTrigger())
   {
      if (payload == "1")
      {
         HA_DISCOVER = true;  // start discovery in next MQTT_MAIN()
      }
   }
}

