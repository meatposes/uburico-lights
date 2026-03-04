# CF56Pro LED — Quick Reference

## Build

```bash
make
```

## Load & Test

```bash
# Load the kernel module
sudo insmod orico_cf56pro_leds.ko

# Check it loaded
dmesg | grep cf56pro
lsmod | grep cf56pro

# Verify LEDs (lights each one for 3s)
sudo ./cf56pro_led_test probe

# Other test commands
sudo ./cf56pro_led_test status
sudo ./cf56pro_led_test set 1 blue on
sudo ./cf56pro_led_test all both off
sudo ./cf56pro_led_test sweep
sudo ./cf56pro_led_test test
```

## Find Your HCTLs

```bash
ls /sys/class/scsi_device/
```

After updating HCTLs in `cf56pro-ledd.c`, rebuild:

```bash
make
```

## Install

```bash
sudo make install

# Auto-load module on boot
echo 'orico_cf56pro_leds' | sudo tee /etc/modules-load.d/cf56pro-leds.conf

# Enable daemon
sudo make enable
```

## Day-to-Day

```bash
# Restart after config change
sudo make restart

# Tail logs
sudo make logs
# or
journalctl -u cf56pro-ledd -f

# Run daemon in foreground for debugging
sudo systemctl stop cf56pro-ledd
sudo cf56pro-ledd -f
# Ctrl-C to stop, then:
sudo systemctl start cf56pro-ledd
```

## Unload / Uninstall

```bash
# Unload module (without uninstalling)
sudo rmmod orico_cf56pro_leds

# Full uninstall
sudo make disable
sudo make uninstall
```

## If LEDs Don't Respond

```bash
# Check MMIO regions
cat /proc/iomem | grep -i gpio

# Check kernel messages
dmesg | grep cf56pro

# Check pinctrl debug
ls /sys/kernel/debug/pinctrl/
```
