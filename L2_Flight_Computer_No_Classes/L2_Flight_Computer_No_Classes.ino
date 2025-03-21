/*
   This code will run the Design Methods HPR Team 2 flight computer.
   Its purpose is to log data onto an SD card and transmit data to the ground.
*/

#define DEBUG

#ifdef DEBUG
void dpln(char* msg) {
  Serial.println(msg);
}
#endif

/*  Library Declarations  */

#include <Arduino_CRC32.h>
#include <SPI.h>
#include <LoRa.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>
#include "FS.h"
#include "SD.h"
#include <TinyGPS++.h>
#include <MS5611.h>
#include <ESP32Servo.h>

Arduino_CRC32 crc32;

char MSBaroArray[50];
char RRC3_Data[20] = {};
char GPS_Data[50] = {};
int LEDPin = 32;
char imuArray[100];
char GPSArray[120];
char dirname[] = "/data000";

int startTime = 0;

bool launchProceedure = true;                                                               // If ESP32 reboots, it will recommence launch proceedure by default
bool SDStatus = true;
bool MPUStatus = true;
bool GPSLock = false;
bool BaroStatus = false;
bool newGPSData = false;
bool newBaroData = false;
bool newMSBaroData = false;
bool sendGPS = false;
bool calibrateMPUFlag = false;
bool motorArmed = true;
bool motorTesting = true;

// NOTE: FILL THIS STUFF IN ONCE ESC IS CONFIGURED
#define ESC_MIN_PPM        1148
#define ESC_MAX_PPM        1832
#define ESC_NEUTRAL_PPM    1488

#define ESC_ARM_SIGNAL     1000  // might need to be changed
#define ESC_ARM_TIME       2000  // might need to be changed
#define ESC_OPERATING_FREQ 24000
#define ESC_CONTROL_PIN    33
Servo esc;

float speed = 1488.0;

typedef struct {
  float prev_error;
  float integral;
  float max_integral;
  float Kp;
  float Kt;
  float Kd;
} PID_data;

typedef struct {
  unsigned long totalMillis;
  int accX;
  int accY;
  int accZ;
  int gyroX;
  int gyroY;
  int gyroZ;
  int tempC;
} imu;

typedef struct {
  unsigned long totalMillis;
  signed long latt;
  signed long longi;
  int alt;
  unsigned long tStamp;
  int speedMps;
  int heading;
  int numSat;
} GPS;

typedef struct {
  unsigned long totalMillis;
  float altitude;
  float velocity;
  float tempF;
  float battVolt;
  char event[5];
} RRC3;

typedef struct {
  unsigned long totalMillis;
  float altitude;
  float pressure;
  float tempC;
} MS5611Data;

typedef union _send_imu {
  imu imuData;
  byte imuByteArray[sizeof(imu)];
} send_imu;

typedef union _send_GPS {
  GPS GPSData;
  byte GPSByteArray[sizeof(GPS)];
} send_GPS;

typedef union _send_RRC3baro {
  RRC3 RRC3baroData;
  byte baroByteArray[sizeof(RRC3)];
} send_RRC3baro;

typedef union _send_MSbaro {
  MS5611Data MSbaroData;
  byte MSbaroByteArray[sizeof(MS5611Data)];
} send_MSbaro;

float timeStamp = 0;

// Lora module pins
const int csPin = 15;
const int resetPin = 16;
const int irqPin = 27;
const long frequency = 915E6;
SPIClass rfSPI(HSPI);

char a[20], b[20], c[20], d[20], e[20], f[20], g[20], h[20];               //  Initalise char containers

char IMUFileName[9] = "";
char baroFileName[10] = "";
char GPSFileName[9] = "";

TinyGPSPlus gps;
Adafruit_MPU6050 mpu;
TaskHandle_t Task1;
TaskHandle_t Task2;
TaskHandle_t Task3;

PID_data pid;

imu imuData;
GPS GPSData;
RRC3 RRC3baroData;
MS5611Data MSBaro;

send_imu imuUnion;
send_GPS GPSUnion;
send_RRC3baro RRC3baroUnion;
send_MSbaro MSBaroUnion;

File RRC3baroFile;
File MSbaroFile;
File IMUDataFile;
File GPSDataFile;

const int GPS_RX = 35;
const int GPS_TX = 17;
const int SERIAL1_RX = 34;
const int SERIAL1_TX = 4;
const int GPS_BAUD = 115200;

float accXoffset = 0.723231;
float accYoffset = -0.082033;
float accZoffset = -0.022410;

float gyroXoffset = -0.056866;
float gyroYoffset = -0.016219;
float gyroZoffset = -0.256082;

SemaphoreHandle_t xSemaphore = NULL;

MS5611 ms5611;                                                   // Initalise MS5611 library

// PID stuff
float SETPOINT_ROLL = 0;
float error = 0;
float pid_out = 0;

float dt = 0;
float timeNow = 0;
float lastTime = 0;

void setup() {
  Serial.begin(115200);
  pinMode(LEDPin, OUTPUT);
  Wire.begin();
  Serial2.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  Serial1.begin(9600, SERIAL_8N1, SERIAL1_RX, SERIAL1_TX);
  xSemaphore = xSemaphoreCreateMutex();             // Create semaphore to protect data while writing  
  pinMode(irqPin, INPUT);
  
  esc.attach(ESC_CONTROL_PIN, ESC_MIN_PPM, ESC_MAX_PPM);
  initESC();
  //create a task that will a executed in the Task1code() function, with priority 1 and executed on core 0
  xTaskCreatePinnedToCore(
    Task1code,   /* Task function. */
    "Task1",     /* name of task. */
    10000,       /* Stack size of task */
    NULL,        /* parameter of the task */
    1,           /* priority of the task */
    &Task1,      /* Task handle to keep track of created task */
    0);          /* pin task to core 0 */
  delay(500);

  //create a task that will be executed in the Task2code() function, with priority 1 and executed on core 1
  xTaskCreatePinnedToCore(
    Task2code,   /* Task function. */
    "Task2",     /* name of task. */
    7000,       /* Stack size of task */
    NULL,        /* parameter of the task */
    1,           /* priority of the task */
    &Task2,      /* Task handle to keep track of created task */
    1);          /* pin task to core 1 */
  delay(500);

  xTaskCreatePinnedToCore(
    Task3code,
    "Task3",
    3000,
    NULL,
    1,
    &Task3,
    1
  );
  delay(500);
}

void loop() {

}

//Task1code:
/*
  This code runs the data trasmit to the ground station
*/
void Task1code( void * pvParameters ) {

  LoRa.setPins(csPin, resetPin, irqPin);
  LoRa.setSPI(rfSPI);
  while (!LoRa.begin(frequency)) {
    Serial.println("LoRa failed");
  }
  // register the receive callback
  LoRa.onReceive(onReceive);
  // put the radio into receive mode
  LoRa.receive();

  for (;;) {
    while (launchProceedure) {
      //    Transmit data to ground
      uint32_t imuChecksum = 0;
      byte IMUtransmittBuffer[sizeof(imuUnion.imuByteArray)];
      if ( xSemaphoreTake( xSemaphore, ( TickType_t ) 10 ) == pdTRUE ) {
        for (int i = 0; i < sizeof(imuUnion.imuByteArray); i++) {             // Take semaphore and copy protected memory into temporary buffer
          IMUtransmittBuffer[i] = imuUnion.imuByteArray[i];
        }
        xSemaphoreGive( xSemaphore );
      }
      //      Serial.println("Sending IMU data");
      LoRa.beginPacket();
      LoRa.write(0x5);
      imuChecksum = crc32.calc((uint8_t*)&IMUtransmittBuffer, sizeof(IMUtransmittBuffer));
      LoRa.write((uint8_t*)&IMUtransmittBuffer, sizeof(IMUtransmittBuffer));
      // Send 32bit CRC
      LoRa.write(imuChecksum);
      LoRa.write((imuChecksum >> 8) & 0xFF);
      LoRa.write((imuChecksum >> 16) & 0xFF);
      LoRa.write((imuChecksum >> 24) & 0xFF);
      LoRa.endPacket(false);

      memset(IMUtransmittBuffer, 0, sizeof(IMUtransmittBuffer));

      if (newBaroData) {
        //        Serial.println("Sending RRC3 data");
        LoRa.beginPacket();
        LoRa.write(0x3);
        //LoRa.println(RRC3_Data);// Send header byte
        //LoRa.write((uint8_t*)&baroData, sizeof(baroData));
        LoRa.endPacket(false);
        LoRa.flush();
        newBaroData = false;
      }

      byte MStransmittBuffer[sizeof(MSBaroUnion.MSbaroByteArray)];
      if (newMSBaroData) {
        //        Serial.println("Sending baro data");
        if ( xSemaphoreTake( xSemaphore, ( TickType_t ) 10 ) == pdTRUE ) {
          for (int i = 0; i < sizeof(MSBaroUnion.MSbaroByteArray); i++) {             // Take semaphore and copy protected memory into temporary buffer
            MStransmittBuffer[i] = MSBaroUnion.MSbaroByteArray[i];
          }
          xSemaphoreGive( xSemaphore );
          uint32_t MSBaroChecksum = 0;
          MSBaroChecksum = crc32.calc((uint8_t*)&MStransmittBuffer, sizeof(MStransmittBuffer));
          LoRa.beginPacket();
          LoRa.write(0x4);
          // for (int i = 0; i < sizeof(MStransmittBuffer); i++) {
          //   Serial.print(MStransmittBuffer[i], HEX);
          // }
          // Serial.println("");

          LoRa.write((uint8_t*)&MStransmittBuffer, sizeof(MStransmittBuffer));
          // Send 32bit CRC
          LoRa.write(MSBaroChecksum);
          LoRa.write((MSBaroChecksum >> 8) & 0xFF);
          LoRa.write((MSBaroChecksum >> 16) & 0xFF);
          LoRa.write((MSBaroChecksum >> 24) & 0xFF);
          LoRa.endPacket();
          LoRa.flush();
        }
        memset(MStransmittBuffer, 0, sizeof(MStransmittBuffer));
        newMSBaroData = false;
      }

      byte GPStransmittBuffer[sizeof(GPSUnion.GPSByteArray)];
      if (true /*sendGPS*/) {     // Allow resending of GPS packets so that missed packets can be resent
        //        Serial.println("Sending GPS data");
        if ( xSemaphoreTake( xSemaphore, ( TickType_t ) 10 ) == pdTRUE ) {
          for (int i = 0; i < sizeof(GPSUnion.GPSByteArray); i++) {             // Take semaphore and copy protected memory into temporary buffer
            GPStransmittBuffer[i] = GPSUnion.GPSByteArray[i];
          }
          xSemaphoreGive( xSemaphore );
          uint32_t GPSChecksum = 0;
          GPSChecksum = crc32.calc((uint8_t*)&GPStransmittBuffer, sizeof(GPStransmittBuffer));
          LoRa.beginPacket();
          LoRa.write(0x6);
          LoRa.write((uint8_t*)&GPStransmittBuffer, sizeof(GPStransmittBuffer));
                  //  Serial.println(sizeof(GPStransmittBuffer) + sizeof(GPSChecksum));
          // Send 32bit CRC
          LoRa.write(GPSChecksum);
          LoRa.write((GPSChecksum >> 8) & 0xFF);
          LoRa.write((GPSChecksum >> 16) & 0xFF);
          LoRa.write((GPSChecksum >> 24) & 0xFF);
          LoRa.endPacket(false);
          LoRa.flush();
          sendGPS = false;
          //          Serial.println(GPSChecksum, HEX);
        }
        memset(GPStransmittBuffer, 0, sizeof(GPStransmittBuffer));
      }
      vTaskDelay(500);
    }
  }
}


//Task2code:
/*
  This code runs the data reading and logging to the SD card
*/
void Task2code( void * pvParameters ) {
  while (!SD.begin()) {
    Serial.println("Card failed, or not present");
    errorBlink(1);
  }

  dpln("Starting MPU6050");
  while (!mpu.begin()) {
    Serial.println("Failed to find MPU6050 chip");
    errorBlink(2);
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_16_G);
  mpu.setGyroRange(MPU6050_RANGE_1000_DEG);
  
  dpln("Starting MS5611");
  while (!ms5611.begin(MS5611_ULTRA_HIGH_RES)) {
    Serial.println(F("Error connecting to barometer"));
    errorBlink(1);
  }
  dpln("Creating file name");
  for (int i = 0; i < 100; i++) {
    Serial.println("Please for the love of god");
    if (SD.exists(dirname)) {
      dirname[strlen(dirname) - 3] = '\0';
      sprintf(dirname, "%s%03d", dirname, i);
    } else {
      Serial.println("I");
      sprintf(dirname, "%s%03d", dirname, i);
      Serial.println("Love");
      dirname[strlen(dirname) - 3] = '\0';
      Serial.println("Print statement");
      SD.mkdir(dirname);
      Serial.println("Debugging");
      break;
    }
  }
  Serial.println("You alive?");
  strcpy(baroFileName, dirname);
  char baroF[12] = "/C3baro.csv";
  strcat(baroFileName, baroF);
  Serial.println("This is getting a bit ridiculous");
  RRC3baroFile = SD.open(baroFileName, FILE_WRITE);
  Serial.println("Surely not");
  String AltimeterHead = "Time (ms), Altitude (m), Velocity (m/s), Temperature (degC), Events";
  RRC3baroFile.print(AltimeterHead);
  Serial.println("I'm going to cry");
  RRC3baroFile.flush();
  Serial.println("Still alive??");
  strcpy(baroFileName, dirname);
  strcpy(baroF, "/MSbaro.csv");
  strcat(baroFileName, baroF);
  MSbaroFile = SD.open(baroFileName, FILE_WRITE);
  AltimeterHead = "Time (ms), Altitude (m), Pressure (Pa), Temperature (degC)";
  MSbaroFile.println(AltimeterHead);
  MSbaroFile.flush();

  strcpy(IMUFileName, dirname);
  char imuF[10] = "/imu.csv";
  strcat(IMUFileName, imuF);
  IMUDataFile = SD.open(IMUFileName, FILE_WRITE);
  String imuDataHead = "Time (ms), XAccel (m/s^2), YAccel (m/s^2), ZAccel (m/s^2), X Angular Rate (rad/s), Y Angular Rate (rad/s), Z Angular Rate (rad/s)";
  IMUDataFile.println(imuDataHead);
  IMUDataFile.flush();

  strcpy(GPSFileName, dirname);
  char GPSF[10] = "/GPS.csv";
  strcat(GPSFileName, GPSF);
  GPSDataFile = SD.open(GPSFileName, FILE_WRITE);
  String GPSDataHead = "Lattitude , Longitude, Altitude (m), Timestamp (HHMMSSCC), Number of Satellites, Heading (deg), Speed (m/s)";
  GPSDataFile.println(GPSDataHead);
  GPSDataFile.flush();
  Serial.println("how about now?");
  //   Setup GPS
  Serial2.print( F("$PMTK251,115200*1F\r\n") );                         // set 115200 baud rate
  Serial2.flush();                                                      // wait for the command to go out
  delay(100);                                                           // wait for the GPS device to change speeds
  Serial2.end();                                                        // empty the input buffer, too

  Serial2.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);                  // use the new baud rate
  Serial2.print( F(",1000*2$PMTK220F\r\n") );                            // set 10Hz update rate - First number is position fix interval in ms



  /********/
//    delay(5000);
//    calibrateMPU();                                                                                                                      // <------------ Delete before launch
  /********/

  //    This is the Core 2 main loop
  for (;;) {
    while (launchProceedure) {

      //  Get imu datat
      sensors_event_t acc, gyr, temp;
      mpu.getEvent(&acc, &gyr, &temp);                  // Blocking call -> check if new data and only poll if there is
      // Serial.printf("%f, %f, %f\r\n", gyr.gyro.x, gyr.gyro.y, gyr.gyro.z);
      if ( xSemaphore != NULL )
      {
        /* See if we can obtain the semaphore.  If the semaphore is not
          available wait 10 ticks to see if it becomes free. */
        if ( xSemaphoreTake( xSemaphore, ( TickType_t ) 10 ) == pdTRUE )
        {
          // Serial.println("Do we be working???");
          imuUnion.imuData.totalMillis = millis();         // Log aquisition time

          imuUnion.imuData.accX = (acc.acceleration.x - accXoffset) * 100;
          imuUnion.imuData.accY = (acc.acceleration.y - accYoffset) * 100;
          imuUnion.imuData.accZ = (acc.acceleration.z - accZoffset) * 100;

          imuUnion.imuData.gyroX = (gyr.gyro.x - gyroXoffset) * 100;
          imuUnion.imuData.gyroY = (gyr.gyro.y - gyroYoffset) * 100;
          imuUnion.imuData.gyroZ = (gyr.gyro.z - gyroZoffset) * 100;
          imuUnion.imuData.tempC = temp.temperature * 100;

          // Serial.println(imuUnion.imuData.gyroX);
          xSemaphoreGive( xSemaphore );                             // Release semaphore
        }
      }

      dtostrf(millis(), 10, 0, a);                                         //  Convert to string
      dtostrf(((float)acc.acceleration.x - accXoffset), 4, 2, b);
      dtostrf(((float)acc.acceleration.y - accYoffset), 4, 2, c);
      dtostrf(((float)acc.acceleration.z - accZoffset), 4, 2, d);
      dtostrf(((float)gyr.gyro.x - gyroXoffset), 4, 2, e);
      dtostrf(((float)gyr.gyro.x - gyroYoffset), 4, 2, f);
      dtostrf(((float)gyr.gyro.x - gyroZoffset), 4, 2, g);
      dtostrf(((float)temp.temperature), 4, 2, h);

      sprintf(imuArray, "%s,%s,%s,%s,%s,%s,%s,%s", a, b, c, d, e, f, g, h);     //  Convert to character array

      //  Write imu data to SD
      // dpln(imuArray);
      IMUDataFile.println(imuArray);

      // *************************************************************************  Barometer *********************************************************************************
      char RRC3_Data[100];

      static byte ndx = 0;                    // Read binary uart data into buffer -> RRC3_Data
      if (Serial1.available() > 0) {
        while (Serial1.available() > 0) {
          char inByte = Serial1.read();
            // Serial.print(inByte);
          if (inByte != '\n') {
            newBaroData = true;
            RRC3_Data[ndx] = inByte;
            ndx++;
          }
          else break;
        }
      }
      ndx = 0;
      if (newBaroData) {
        // RRC3baroData.totalMillis = millis();
        // int i = 0;
        // while (RRC3_Data[i] != 0x2C) {    // Discard first packet
        //   i++;
        // }
        // char buff[20];
        // while (RRC3_Data[i] != 0x2C) {    // Extract altitude data
        //   buff[i] = RRC3_Data[i];
        //   i++;
        // }
        // RRC3baroData.altitude = float(atof(buff));
        // memset(buff, 0, sizeof(buff));

        // while (RRC3_Data[i] != 0x2C) {    // Extract velocity data
        //   buff[i] = RRC3_Data[i];
        //   i++;
        // }
        // RRC3baroData.velocity = float(atof(buff));
        // memset(buff, 0, sizeof(buff));

        // while (RRC3_Data[i] != 0x2C) {    // Extract temperature data
        //   buff[i] = RRC3_Data[i];
        //   i++;
        // }
        // RRC3baroData.tempF = float(atof(buff));
        // memset(buff, 0, sizeof(buff));

        // while (RRC3_Data[i] != 0x2C) {
        //   buff[i] = RRC3_Data[i];
        //   i++;
        // }
        // strcpy(RRC3baroData.event, buff);
        // memset(buff, 0, sizeof(buff));

        // while (RRC3_Data[i] != 0x2C) {
        //   buff[i] = RRC3_Data[i];
        //   i++;
        // }
        // RRC3baroData.battVolt = float(atof(buff));
        // memset(buff, 0, sizeof(buff));

        // Write to SD card
        char print_data[100];
        sprintf(print_data, "%lu,%lu,%s", millis(), micros(), RRC3_Data);
        RRC3baroFile.println(print_data);
        // Serial.println(print_data);
        memset(RRC3_Data, 0, sizeof(RRC3_Data));          //  Empty data array
        newBaroData = false;
      }
      

      if (Serial2.available() > 0) {
        while (Serial2.available() > 0) {
          char b = Serial2.read();
          gps.encode(b);
          // Serial.print(b);
          if (gps.location.isValid()) {
            if ( xSemaphoreTake( xSemaphore, ( TickType_t ) 10 ) == pdTRUE )
            {
              newGPSData = true;
              GPSUnion.GPSData.totalMillis = millis();
              GPSUnion.GPSData.latt = gps.location.lat() * 1000000;
              GPSUnion.GPSData.longi = gps.location.lng() * 1000000;
              GPSUnion.GPSData.alt = gps.altitude.meters();
              GPSUnion.GPSData.tStamp = gps.time.value();
              GPSUnion.GPSData.numSat = gps.satellites.value();
              GPSUnion.GPSData.heading = gps.course.deg();
              GPSUnion.GPSData.speedMps = gps.speed.mps() * 100;
              xSemaphoreGive( xSemaphore );
            }
          }
          else newGPSData = false;
        }
      }

      // Update every 2Hz


      if (newGPSData) {
        sendGPS = true;
        char lt[20], ln[20], al[20], ts[20], ns[20], hd[20], sm[20];      //  Initalise char container
        dtostrf(((float)gps.location.lat()), 10, 6, lt);                                        //  Convert to string
        dtostrf(((float)gps.location.lng()), 10, 6, ln);
        dtostrf(gps.altitude.meters(), 5, 0, al);
        dtostrf(gps.time.value(), 10, 0, ts);
        dtostrf(gps.satellites.value(), 2, 0, ns);
        dtostrf(gps.course.deg(), 4, 0, hd);
        dtostrf(((float)gps.speed.mps()) / 100, 6, 2, sm);
        memset(GPSArray, 0, sizeof(GPS_Data));
        sprintf(GPSArray, "%s,%s,%s,%s,%s,%s,%s", lt, ln, al, ts, ns, hd, sm);    //  Convert to character array
        dpln(GPSArray);
        GPSDataFile.println(GPSArray);

      }
      newGPSData = false;

      if (calibrateMPUFlag) {
        calibrateMPU();
        calibrateMPUFlag = false;
      }

      // Read baro every 100ms
      if (millis() - startTime > 100) {
        startTime = millis();
        if (xSemaphoreTake(xSemaphore, (TickType_t)10) == pdTRUE) {
          MSBaroUnion.MSbaroData.totalMillis = millis();
          MSBaroUnion.MSbaroData.tempC = ms5611.readTemperature(true);                         // Returns temperature in C
          MSBaroUnion.MSbaroData.pressure = ms5611.readPressure(true);                         // Returns pressure in Pascals
          MSBaroUnion.MSbaroData.altitude = ms5611.getAltitude(MSBaroUnion.MSbaroData.pressure);
        }
        xSemaphoreGive(xSemaphore);
        //        Serial.println(ms5611.getAltitude(MSBaroUnion.MSbaroData.pressure));
        dtostrf(millis(), 10, 0, a);                                         //  Convert to string
        dtostrf(((float)MSBaroUnion.MSbaroData.altitude), 5, 2, b);
        dtostrf(((float)MSBaroUnion.MSbaroData.pressure), 4, 0, c);
        dtostrf(((float)MSBaroUnion.MSbaroData.tempC), 4, 2, d);
        char MSBaroArray[50];
        memset(GPSArray, 0, sizeof(GPS_Data));
        sprintf(MSBaroArray, "%s,%s,%s,%s", a, b, c, d);     //  Convert to character array
        MSbaroFile.println(MSBaroArray);
        newMSBaroData = true;
      }

      IMUDataFile.flush();
      RRC3baroFile.flush();
      MSbaroFile.flush();
      GPSDataFile.flush();
    }
  }
}

//Task3code:
/*
  This code handles controlling the motor and running the PID loop
*/
void Task3code( void * pvParameters ) {
  // Serial.println("Hel me please");
  // set parameters for the control system
  pid.Kp = -2.5;
  pid.Kd = 3;
  pid.Kt = -0.001;
  pid.max_integral = 300;

  int start_time = millis();

  for (;;) {
    if (motorTesting) {
      if (millis() - start_time > 15000) {
        writeSpeed(ESC_NEUTRAL_PPM);
        continue;
      }
    } else if (!motorArmed) {
      writeSpeed(ESC_NEUTRAL_PPM);  // ensure we're not spinning the motor when it is not armed
      continue;
    }

    error = SETPOINT_ROLL - imuUnion.imuData.gyroX;

    if (error < 5 & error > -5) {  // ignore noise
      error = 0;
    }

    lastTime = timeNow;
    timeNow = ((float) millis()) / 1000;
    dt = timeNow - lastTime;

    pid_out = PID(error, dt);  // maybe clamp output so it can't ramp up speed toooooo fast

    speed += pid_out * dt;  // take the integral of PID output, add to speed
    writeSpeed((int)(speed + 0.5));  // send instruction to ESC

    char stuff[200];
    sprintf(stuff, "Error: %f, Speed: %f, Pidout: %f, Dt: %f, rounded: %d", error, speed, pid_out, dt, (int)(speed + 0.5));
    Serial.println(stuff);

    delay(10);
  }
}

void onReceive(int packetSize) {
  Serial.println("Recieved packet");
  // read packet
  int inByte;
  for (int i = 0; i < packetSize; i++) {
    inByte = LoRa.read();
  }
  Serial.print(inByte);
  if (inByte == 0x1) {
    // Start launch proceedure
    launchProceedure = true;

  }
  else if (inByte == 2) {
    // Sensor status - Send array of bytes to reflect states
  }
  else if (inByte == 3) {
    calibrateMPUFlag = true;
    LoRa.beginPacket();
    LoRa.print("MPU calibrated");
    LoRa.endPacket(true);
  }
  else if (inByte == 0) {
    // Shut down sensor
    launchProceedure = false;
    Serial.println("Shutting down sensor");
  }
  else if (inByte == 4) {
    // arms DC motor, allowing it to spin when told
    motorArmed = true;
  }
}



void errorBlink(int scaleFactor) {
  digitalWrite(LEDPin, HIGH);
  delay(500 * scaleFactor);
  digitalWrite(LEDPin, LOW);
  delay(500 * scaleFactor);
}

void calibrateMPU() {
  double accelXAvg = 0;
  double accelYAvg = 0;
  double accelZAvg = 0;
  double gyroXAvg = 0;
  double gyroYAvg = 0;
  double gyroZAvg = 0;

  int num_sum = 500;
  Serial.println("*************** STARTING CALIBRATION ROUTING ***************");
  for (int i = 0; i < num_sum; i++) {
    sensors_event_t acc, gyr, temp;
    mpu.getEvent(&acc, &gyr, &temp);
    accelXAvg += acc.acceleration.x;
    accelYAvg += acc.acceleration.y;
    accelZAvg += acc.acceleration.z;
    gyroXAvg += gyr.gyro.x;
    gyroYAvg += gyr.gyro.y;
    gyroZAvg += gyr.gyro.z;
  }
  accXoffset = accelXAvg / num_sum;
  accYoffset = accelYAvg / num_sum;
  accZoffset = accelZAvg / num_sum;
  gyroXoffset = gyroXAvg / num_sum;
  gyroYoffset = gyroYAvg / num_sum;
  gyroZoffset = gyroZAvg / num_sum;
  
  Serial.printf("X acceleraton offset: %0.6f\r\n", accXoffset);
  Serial.printf("Y acceleraton offset: %0.6f\r\n", accYoffset);
  Serial.printf("Z acceleraton offset: %0.6f\r\n", accZoffset);

  Serial.printf("X gyro offset: %0.6f\r\n", gyroXoffset);
  Serial.printf("Y gyro offset: %0.6f\r\n", gyroYoffset);
  Serial.printf("Z gyro offset: %0.6f\r\n", gyroZoffset);
}

void initESC() {
  // esc.setPeriodHertz(ESC_OPERATING_FREQ);
  // delay(10);
  esc.writeMicroseconds(1000);
  delay(1000);
  esc.writeMicroseconds(1200);
  delay(1500);
  esc.writeMicroseconds(ESC_NEUTRAL_PPM);
  delay(10);
}

void writeSpeed(int speed) {
  // may need some additional logic to include a deadzone, etc
  if (speed > ESC_MAX_PPM) {
    speed = ESC_MAX_PPM;
  } else if (speed < ESC_MIN_PPM) {
    speed = ESC_MIN_PPM;
  } else if (speed > ESC_NEUTRAL_PPM - 20 & speed < ESC_NEUTRAL_PPM + 20) {  // deadzone, stop stutter when basically stationary
    speed = ESC_NEUTRAL_PPM;
  }

  esc.writeMicroseconds(speed);
}

float PID(float error, float dt) {
  float derivative = (error - pid.prev_error) / dt;
  pid.integral += error * dt;

  if (pid.integral > pid.max_integral) {
    pid.integral = pid.max_integral;
  } else if (pid.integral < -pid.max_integral) {
    pid.integral = -pid.max_integral;
  }

  pid.prev_error = error;

  return pid.Kp * error + pid.Kt * pid.integral + pid.Kd * derivative;

  // pid.prev_error = error;

  // return pid.Kp * error + pid.Kd * derivative;
}