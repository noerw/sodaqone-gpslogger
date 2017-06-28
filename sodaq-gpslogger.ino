/*
 *
 * GPS SD logger based on SODAQ One with ONEBase + Adafruit SD breakout
 *
 * red  LED on: no GPS fix
 * blue LED on: SD card error
 * green LED flash: write to logfile
 *

 The circuit:
  * SD card attached to SPI bus as follows:
 ** MOSI / DI - pin D10
 ** MISO / DO - pin D8
 ** CLK - pin D7 (D11 if not on ONEBase)
 ** CS - pin D9

  MIT License

  Copyright (c) 2017 Norwin Roosen, Felix Erdmann

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
 */

#include <SPI.h>
#include <SD.h>

#include "Sodaq_UBlox_GPS.h"
#include "RTCZero.h"
// Water temperature libraries
#include "OneWire.h"
#include "DallasTemperature.h"

#define SD_CHIPSELECT 9
#define ONE_WIRE_BUS 2

// change to Serial to disable log output. when set to SerialUSB, log output is given, but device only works when connected via USB
#define DEBUG_OUT Serial

#define MEASURE_INTERVAL 60000        // measure every minute
#define GPSIDLE_INTERVAL (60000 * 15) // get fix every 15 mins when not moving
#define MOVE_THRESHOLD 60             // consider we have moved if distance to last fix is more than 60 meters

// declare water temperature sensor
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
RTCZero rtc;

File logfile;
String logfile_path;
uint32_t cyclestart = 0;
double lastLat = 0;
double lastLng = 0;
long gpsupdate_scheduled = 0;
double temperature = 0;

/**
 * rollover safe implementation, with an optional offset
 */
void adaptive_delay(uint32_t duration, uint32_t offset = 0) {
  unsigned long start = millis();
  for (;;) {
    unsigned long now = millis();
    unsigned long elapsed = now - start + offset;
    if (elapsed >= duration) return;
  }
}

/**
 * copied from SD example sketch
 * created  28 Mar 2011
 * by Limor Fried
 * modified 9 Apr 2012
 * by Tom Igoe
 */
bool check_sdcard (uint8_t chipSelect) {
  // set up variables using the SD utility library functions:
  Sd2Card card;
  SdVolume volume;
  SdFile root;

  DEBUG_OUT.print("\nInitializing SD card...");

  // we'll use the initialization code from the utility libraries
  // since we're just testing if the card is working!
  if (!card.init(SPI_HALF_SPEED, chipSelect)) {
    DEBUG_OUT.println("initialization failed. Things to check:");
    DEBUG_OUT.println("* is a card inserted?");
    DEBUG_OUT.println("* is your wiring correct?");
    DEBUG_OUT.println("* did you change the chipSelect pin to match your shield or module?");
    return false;
  } else {
    DEBUG_OUT.println("Wiring is correct and a card is present.");
  }

  // print the type of card
  DEBUG_OUT.print("\tCard type: ");
  switch (card.type()) {
    case SD_CARD_TYPE_SD1:
      DEBUG_OUT.println("SD1");
      break;
    case SD_CARD_TYPE_SD2:
      DEBUG_OUT.println("SD2");
      break;
    case SD_CARD_TYPE_SDHC:
      DEBUG_OUT.println("SDHC");
      break;
    default:
      DEBUG_OUT.println("Unknown");
  }

  // Now we will try to open the 'volume'/'partition' - it should be FAT16 or FAT32
  if (!volume.init(card)) {
    DEBUG_OUT.println("Could not find FAT16/FAT32 partition.\nMake sure you've formatted the card");
    return false;
  }

  // print the type and size of the first FAT-type volume
  uint32_t volumesize;
  DEBUG_OUT.print("\tVolume type is FAT");
  DEBUG_OUT.println(volume.fatType(), DEC);

  volumesize = volume.blocksPerCluster();  // clusters are collections of blocks
  volumesize *= volume.clusterCount();     // we'll have a lot of clusters
  volumesize *= 512;                       // SD card blocks are always 512 bytes
  DEBUG_OUT.print("\tVolume size (bytes): ");
  DEBUG_OUT.print(volumesize);
  DEBUG_OUT.print(" bytes (");
  volumesize /= 1024;
  volumesize /= 1024;
  DEBUG_OUT.print(volumesize);
  DEBUG_OUT.println(" MB)");

  // TODO: show remaining bytes?

  //SerialUSB.println("\tFiles found on the card (name, date and size in bytes): ");
  //root.openRoot(volume);
  // list all files in the card with date and size
  //root.ls(LS_R | LS_DATE | LS_SIZE);

  return true;
}

/**
 * copied from TinyGPS++.cpp (https://github.com/mikalhart/TinyGPSPlus)
 * Copyright (C) 2008-2013 Mikal Hart All rights reserved. License: GPL
 */
double distanceBetween(double lat1, double long1, double lat2, double long2) {
  // returns distance in meters between two positions, both specified
  // as signed decimal-degrees latitude and longitude. Uses great-circle
  // distance computation for hypothetical sphere of radius 6372795 meters.
  // Because Earth is no exact sphere, rounding errors may be up to 0.5%.
  // Courtesy of Maarten Lamers
  double delta = radians(long1-long2);
  double sdlong = sin(delta);
  double cdlong = cos(delta);
  lat1 = radians(lat1);
  lat2 = radians(lat2);
  double slat1 = sin(lat1);
  double clat1 = cos(lat1);
  double slat2 = sin(lat2);
  double clat2 = cos(lat2);
  delta = (clat1 * slat2) - (slat1 * clat2 * cdlong);
  delta = sq(delta);
  delta += sq(clat2 * sdlong);
  delta = sqrt(delta);
  double denom = (slat1 * slat2) + (clat1 * clat2 * cdlong);
  delta = atan2(delta, denom);
  return delta * 6372795;
}

bool find_gps_fix(uint32_t timeout = 60000) {
  uint32_t start = millis();

  DEBUG_OUT.println(String("waiting for fix... timeout: ") + timeout + String("ms"));

  if (sodaq_gps.scan(true, timeout)) { // keep GPS enabled afterwards
    DEBUG_OUT.println(String("time to find fix: ") + (millis() - start) + String("ms"));

    // (re-)sync RTC
    rtc.setSeconds(sodaq_gps.getSecond());
    rtc.setMinutes(sodaq_gps.getMinute());
    rtc.setHours(sodaq_gps.getHour());
    rtc.setDay(sodaq_gps.getDay());
    rtc.setMonth(sodaq_gps.getMonth());
    rtc.setYear(sodaq_gps.getYear() - 2000);

    return true;
  } else {
    DEBUG_OUT.println("\tNo Fix after timeout");
    return false;
  }
}

void flash_led(uint8_t pin) {
  digitalWrite(pin, LOW);
  delay(100);
  digitalWrite(pin, HIGH);
}

String int2String(int num, size_t width) {
  String out;
  out = num;
  while (out.length() < width) {
    out = String("0") + out;
  }
  return out;
}

String make_logfile_path() {
  // filenames longer than 12 chars cannot be written..??
  return sodaq_gps.getDateTimeString().substring(0, 8) + ".csv";
}

void write_log_to_stream(Print &stream) {
  // ISODate
  stream.print(int2String(2000 + rtc.getYear(), 4));
  stream.print('-');
  stream.print(int2String(rtc.getMonth(), 2));
  stream.print('-');
  stream.print(int2String(rtc.getDay(), 2));
  stream.print('T');
  stream.print(int2String(rtc.getHours(), 2));
  stream.print(':');
  stream.print(int2String(rtc.getMinutes(), 2));
  stream.print(':');
  stream.print(int2String(rtc.getSeconds(), 2));
  stream.print(',');

  // GPS
  stream.print(String(sodaq_gps.getLat(), 6));
  stream.print(',');
  stream.print(String(sodaq_gps.getLon(), 6));
  stream.print(',');
  stream.print(String(sodaq_gps.getAlt(), 3));
  stream.print(',');
  stream.print(String(sodaq_gps.getHDOP(), 3));
  stream.print(',');
  stream.print(String(sodaq_gps.getNumberOfSatellites()));
  stream.print(',');

  // temperature
  stream.print(temperature);
  stream.println();
}

void setup() {
  // init RGB led, also as startup feedback
  digitalWrite(LED_RED, HIGH);
  pinMode(LED_RED, OUTPUT);
  digitalWrite(LED_GREEN, HIGH);
  pinMode(LED_GREEN, OUTPUT);
  digitalWrite(LED_BLUE, HIGH);
  pinMode(LED_BLUE, OUTPUT);
  flash_led(LED_RED);
  flash_led(LED_GREEN);
  flash_led(LED_BLUE);

  // Open serial communications and wait for port to open
  DEBUG_OUT.begin(9600);
  while (!DEBUG_OUT) ;

  if (!check_sdcard(SD_CHIPSELECT)) {
    digitalWrite(LED_BLUE, LOW);
    while (true) ;
  }

  if (!SD.begin(SD_CHIPSELECT)) {
    DEBUG_OUT.println("can't open SD card, though a card was found...");
    digitalWrite(LED_BLUE, LOW);
    while (true) ;
  }

  rtc.begin();

  // initialize water temperature sensor
  sensors.begin();

  sodaq_gps.init(GPS_ENABLE);
  //sodaq_gps.setDiag(DEBUG_OUT);

  digitalWrite(LED_RED, LOW);
  while (!find_gps_fix(10000)) ; // get first fix & keep GPS enabled afterwards
  lastLat = sodaq_gps.getLat();
  lastLng = sodaq_gps.getLon();
  digitalWrite(LED_RED, HIGH);

  logfile_path = make_logfile_path();

  DEBUG_OUT.print("got fix. ready to write data to ");
  DEBUG_OUT.println(logfile_path);

  // if the logfile does not exist yet, write the csv header
  if (!SD.exists(logfile_path)) {
    logfile = SD.open(logfile_path, FILE_WRITE);
    if (logfile) {
      logfile.println("timestamp,latitude,longitude,altitude,HDOP,satellitecount,temperature");
      logfile.close();
    } else {
      DEBUG_OUT.print("unable to write CSV header to logfile");
      digitalWrite(LED_BLUE, LOW);
      while (true) ;
    }
  }
}

void loop(void) {
  cyclestart = millis();

  // TODO: only update fix, if accelerometer indicates movement?
  // TODO: check battery voltage & blink LED if low?
  if (millis() >= gpsupdate_scheduled) {
    digitalWrite(LED_RED, LOW);
    if (!find_gps_fix()) {
      DEBUG_OUT.print("couldnt get fix");
      return;
    }

    // determine, how far we moved. if less than 20m, reduce update interval
    if (distanceBetween(lastLat, lastLng, sodaq_gps.getLat(), sodaq_gps.getLon()) > MOVE_THRESHOLD) {
      gpsupdate_scheduled = cyclestart + MEASURE_INTERVAL;
    } else {
      gpsupdate_scheduled = cyclestart + GPSIDLE_INTERVAL;
      digitalWrite(GPS_ENABLE, LOW); // save energy for longer interval
    }

    lastLat = sodaq_gps.getLat();
    lastLng = sodaq_gps.getLon();
    digitalWrite(LED_RED, HIGH);
  }

  // measure temperature
  sensors.requestTemperatures();
  temperature = sensors.getTempCByIndex(0);

  write_log_to_stream(DEBUG_OUT);

  // if the file opened okay write GPS fix to it
  logfile = SD.open(logfile_path, FILE_WRITE);
  if (logfile) {
    digitalWrite(LED_BLUE, HIGH);
    DEBUG_OUT.print("writing to logfile ");
    DEBUG_OUT.print(logfile_path);
    DEBUG_OUT.print(" ... ");

    write_log_to_stream(logfile);

    logfile.close();
    flash_led(LED_GREEN);
    DEBUG_OUT.println("done.");
  } else {
    // FIXME: this will never be evaluated, even if the card is removed??
    digitalWrite(LED_BLUE, LOW);
    DEBUG_OUT.println("error opening logfile");
  }

  // adaptive & rollover safe delay, to ensure longrunning & fixed interval
  adaptive_delay(MEASURE_INTERVAL, millis() - cyclestart);
}
