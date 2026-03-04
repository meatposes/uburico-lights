# uburico-lights

**Linux LED driver and monitoring daemon for ORICO NAS hardware.**

Reverse-engineered from proprietary firmware to give you full, native Linux
control of your NAS bay LEDs — no vendor OS, no black-box services, no nonsense.

Currently supported hardware:

| Hardware | Module | Status |
|----------|--------|--------|
| ORICO CF1000 (Minisforum N5A, Alder Lake-N) | `orico_cf1000_leds` | ✅ v1.0 |

---

## What it does

Installs a Linux kernel LED class driver that exposes every LED on the NAS
as a standard sysfs node under `/sys/class/leds/`. A companion daemon then
watches your drives and network and drives the LEDs automatically.

### Bay LEDs (per drive slot)

| State | LED |
|-------|-----|
| Drive present, healthy, in ZFS pool | Solid blue |
| Drive present, SMART warning | Slow red flash (1s) |
| Drive present, SMART failure | Fast red flash (250ms) |
| Drive expected in pool but missing | Solid red |
| Bay empty | Off |

### Network LED

Reflects aggregate traffic across all non-loopback interfaces via /proc/net/dev:

| State | LED |
|-------|-----|
| Outbound traffic (TX) | Blue |
| Inbound traffic (RX) | Red |
| Bidirectional | Purple |
| Idle | Off |

### System LED

Reflects aggregate disk I/O across all sd* and nvme* devices via /proc/diskstats:

| State | LED |
|-------|-----|
| Disk reads | Blue |
| Disk writes | Red |
| Both | Purple |
| Idle | Off |

---

## How it works

The CF1000's bay LEDs are wired directly to Intel Alder Lake-N PCH GPIO pads.
The OEM firmware controlled them via direct MMIO writes to PADCFG0 registers.

This project replaces that entirely:

- **orico_cf1000_leds.ko** maps two physical MMIO regions (0xFD6A0000,
  0xFD6D0000) at module load time and registers 24 led_classdev devices.
  It deliberately bypasses pinctrl-alderlake (which already owns these pads)
  via direct ioremap, the same mechanism the OEM used.

- **cf1000-ledd** is a pure userspace daemon. It reads /proc/diskstats,
  /proc/net/dev, zpool status, and smartctl, then writes brightness values
  to the sysfs nodes the driver exposes. No /dev/mem, no root filesystem
  writes, no kernel modifications beyond the module itself.

---

## Requirements

| Dependency | Purpose |
|------------|---------|
| Linux kernel headers (matching running kernel) | Build the kernel module |
| gcc, make | Build everything |
| zfsutils-linux | ZFS pool membership detection (optional) |
| smartmontools | SMART health monitoring (optional) |

### Install kernel headers

| Distro | Command |
|--------|---------|
| Ubuntu / Debian | sudo apt install linux-headers-$(uname -r) |
| Fedora / RHEL | sudo dnf install kernel-devel-$(uname -r) |
| openSUSE | sudo zypper install kernel-devel |
| Arch Linux | sudo pacman -S linux-headers |

---

## Installation

```bash
cd cf1000
make
sudo make install

# Auto-load module on boot
echo 'orico_cf1000_leds' | sudo tee /etc/modules-load.d/cf1000-leds.conf

# Enable and start the daemon
sudo make enable
```

---

## Configuration

The config file lives at /etc/cf1000-led.conf. It is never overwritten
by updates — your changes are always preserved.

The upstream defaults are always available at:
  /usr/share/cf1000-led/cf1000-led.conf.default

To reset to defaults:
```bash
sudo cp /usr/share/cf1000-led/cf1000-led.conf.default /etc/cf1000-led.conf
sudo systemctl restart cf1000-ledd
```

---

## Operations

### After editing the config

```bash
sudo systemctl restart cf1000-ledd
# or:
cd cf1000 && make restart
```

### Live logs

```bash
journalctl -u cf1000-ledd -f
# or:
cd cf1000 && make logs
```

### Status at a glance

```bash
cd cf1000 && make status
```

### Run in foreground (debugging)

```bash
sudo systemctl stop cf1000-ledd
sudo cf1000-ledd -f
# Ctrl-C exits cleanly and turns off all LEDs
sudo systemctl start cf1000-ledd
```

### Manual LED control

```bash
sudo ./cf1000_led_test status
sudo ./cf1000_led_test set 1 blue on
sudo ./cf1000_led_test all both off
sudo ./cf1000_led_test sweep
sudo ./cf1000_led_test flash
sudo ./cf1000_led_test test
```

---

## Secure Boot

If insmod returns "Operation not permitted", Secure Boot is blocking the
unsigned module. Either disable Secure Boot in BIOS/UEFI, or sign the module
with your enrolled MOK key.

---

## Troubleshooting

**LEDs don't respond after insmod**
Check dmesg | grep cf1000. Verify with lsmod | grep orico.

**Daemon starts but bay LEDs stay off**
Run: sudo cf1000-ledd -f
Watch the startup log for drive discovery and initial states.

**System LED flickering constantly**
ZFS background I/O is likely the cause. Raise io_threshold in
/etc/cf1000-led.conf and restart the daemon.

**Network LED flickering**
Raise net_threshold in /etc/cf1000-led.conf. Default is 64 bytes/tick.

---

## Contributing

Other ORICO NAS units using Intel PCH GPIO for LEDs should be supportable
with a new hardware directory (e.g. cf2000/) following the same structure.
You will need the PADCFG0 register map for your unit — this can be extracted
from the OEM firmware board config file.

Pull requests welcome.

---

## License

GPL v2. See LICENSE.
