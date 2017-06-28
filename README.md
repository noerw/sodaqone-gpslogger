# sodaqone-gpslogger

Firmware for a (solar powered) GPS logger with temperature sensor.
Measures temperature once a minute and writes the geocoded observation to SD card.
If no movement is detected (via GPS distance), GPS updates only every 15 mins.

Each measurement takes ~62 bytes, so 2GB of storage should last for a lifetime ;)

![photo]()

## LED status lights
- red LED on: trying to get GPS position
- blue LED on: SD card error (not inserted or unreadable)
- green LED blink: measurement written

## hardware

- SODAQOne
- SODAQOne ONEBase (not required, but has different pinout)
- Adafruit MicroSD Card Breakout
- Dallas Temperature Sensor
- Solar Panel + LiPo (not required)

## compile & upload firmware
Clone this repo:
```bash
git clone https://github.com/noerw/sodaqone-gpslogger && cd sodaqone-gpslogger
```

Assuming you have Arduino IDE installed, add the following additional board definitions URL to your Arduino config (eg. `~/.arduino15/preferences.txt`, the field `boardsmanager.additional.urls`):
```
http://downloads.sodaq.net/package_sodaq_samd_index.json
```

Then install the board definition for SODAQ once:
```bash
arduino --install-boards "SODAQ:samd"
```

To compile & upload the firmware:
```bash
export SODAQ_PORT=/dev/ttyACM0 # replace with the SerialUSB device of the board
arduino --upload --board SODAQ:samd:sodaq_one_base --port $SODAQ_PORT --preserve-temp-files sodaq-gpslogger.ino
```

## license
MIT, see the respective files.
