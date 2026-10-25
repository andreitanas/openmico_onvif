#!/bin/sh
# IR LEDs and IR cut filter.
#   nightmode.sh on|off   autonight's verdict: night or day
#   nightmode.sh apply    re-apply after uvcd changed the mode
# The mode comes from Protect's IR setting through uvcd (/tmp/ir_mode):
#   auto            follow autonight
#   autoFilterOnly  follow autonight with the IR LEDs kept off
#   on / off        always night / always day
# The resulting state goes to /tmp/night_vision_enabled, which uvcd follows
# to switch the ISP to black and white.
VERDICT=/tmp/autonight_state
STATE=/tmp/night_vision_enabled

gpio_out() {
	[ -d /sys/class/gpio/gpio$1 ] || echo $1 > /sys/class/gpio/export
	echo out > /sys/class/gpio/gpio$1/direction
	echo $2 > /sys/class/gpio/gpio$1/value
}

case "$1" in
on|off) echo "$1" > $VERDICT ;;
apply) ;;
*) echo "usage: $0 on|off|apply" >&2; exit 2 ;;
esac

verdict=$(cat $VERDICT 2>/dev/null)
mode=$(cat /tmp/ir_mode 2>/dev/null)
case "$mode" in
on) night=1 leds=1 ;;
off) night=0 leds=0 ;;
autoFilterOnly) [ "$verdict" = on ] && night=1 || night=0; leds=0 ;;
*) [ "$verdict" = on ] && night=1 || night=0; leds=$night ;;
esac

gpio_out 49 $leds
if [ "$night" != "$(cat $STATE 2>/dev/null)" ]; then
	if [ $night = 1 ]; then
		# Filter out of the light path
		gpio_out 26 0
		gpio_out 25 1
		sleep 1
		gpio_out 25 0
	else
		gpio_out 25 0
		gpio_out 26 1
		sleep 1
		gpio_out 26 0
	fi
	echo $night > $STATE
fi
