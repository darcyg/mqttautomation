# mqttalrm

This projects implements a traditional alarm clock using MQTT.
It consists of several cooperating tools that communicate
via MQTT.

Via retained messages, the MQTT broker provides the storage
for the alarms.

# example use
## binaries

Run these commands (or start with your init system).

	$ mqttalrm -v 'alarms/+' 'alarms/+/+' &
	$ mqttimer -v alarms/+ alarms/+/timer &
	$ mqttnow -v -s /fmtnow &

## MQTT topic layout

* alarms/NAME/alarm	**HH:MM**, alarm time
* alarms/NAME/repeat	**mtwtfss** for active days, **-** when disabled
* alarms/NAME/enable	0 or 1
* alarms/NAME/skip	0 or 1, when 1, the alarm is skipped **once**
* alarms/NAME		**0**, **1** or **snoozed**
* alarms/NAME/timer	ex **1h**. The alarms will turn off after 1h.
* alarms/NAME2		**0** or **1**
* alarms/NAME2/timer	*timer value*, NAME2 acts as a sleep timer
* state/time		**dow, HH:MM:SS**, current system time
* state/time/fmtnow	**%a, %H:%M:%S**, current system time strftime format

# tools
## mqttalrm

* This program listens to all attributes,
* changes the state attribute to **on**
* It will also reset **skip** when the alarm is actually skipped.
* turns off state when the alarm is disabled or changed/rescheduled

## mqttimer

* listens to state & statetimer
* turns off state after the time specified by statetimer

## alarm.html

A web gui using mosquitto websockets.
This will allow you to add/remove alarms, control all sorts of things

The html page added 1 extra **sleeptimer** within the alarms.
mqttoff will pick it up, just as all alarm listeners.
mqttalrm will never raise it due to its +/alarm abscense.
