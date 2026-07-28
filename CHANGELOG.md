## Added support for Android official app TCP/IP protocol for Linux variant repeater management.

Not all features will work, because Android app is designed for companions. What is working is noise floor scan, discover neighbors, console and few other things. Messaging, channels and contacts are unsupported for Linux meshcored over TCP/IP.

## Added local console for management

client app mcore for local management. Because multiple instances can be running then for connecting to correct instance is used linux sockets. those sockets should be in /run/meshcored directory.
For more info see client console app here: https://github.com/mastodon987/mcore_console

## Added SNMP support

Base OID: .1.3.6.1.4.1.62911.1
for more info see external directory

## Added DS18B20 sensor into Telemetry for linux repeater variant.

## Added GPS support for linux variant.

Tested with ATGM336H. running user (meshcore) has to be in dialout group for have permission to read from GPS device.
NMEA format is supported only (no UBX). 
Meshcore instance cannot run concurrently with running gpsd which reads from gps device too.

## Added MQTT support for linux repeater variant.

for compilation with pio use variant linux_repeater_mqtt.

Into MQTT are sent all listened Meshcore Frames with all frame headers and payload. For MQTT->Meshcore is required same format with all headers. In my another repo python-mc-client there is POC.


## Added CLI commands:

set bridge.mqtt.server

set bridge.mqtt.port

set bridge.mqtt.topic

set bridge.mqtt.user (Optional)

set bridge.mqtt.pass (Optional)

