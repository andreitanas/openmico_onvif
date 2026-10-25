#!/bin/sh
# Called by autonight: "on" = night (IR LEDs on, IR cut filter out of the
# light path), "off" = day. uvcd reads the state from /tmp to switch the ISP.
gpio_out() {
	[ -d /sys/class/gpio/gpio$1 ] || echo $1 > /sys/class/gpio/export
	echo out > /sys/class/gpio/gpio$1/direction
	echo $2 > /sys/class/gpio/gpio$1/value
}
case "$1" in
on)
	gpio_out 49 1
	gpio_out 26 0
	gpio_out 25 1
	sleep 1
	gpio_out 25 0
	echo 1 > /tmp/night_vision_enabled
	;;
off)
	gpio_out 49 0
	gpio_out 25 0
	gpio_out 26 1
	sleep 1
	gpio_out 26 0
	echo 0 > /tmp/night_vision_enabled
	;;
esac
