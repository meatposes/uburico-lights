savedcmd_orico_cf1000_leds.mod := printf '%s\n'   orico_cf1000_leds.o | awk '!x[$$0]++ { print("./"$$0) }' > orico_cf1000_leds.mod
