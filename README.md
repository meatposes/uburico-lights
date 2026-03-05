# uburico-lights

**Linux LED driver and monitoring daemon for ORICO NAS hardware.**

Reverse-engineered from inside the Orico OS to give you full, native Linux
control of your NAS bay LEDs.

Currently supported hardware:

| Hardware                                     | Module               | Status                            |
| -------------------------------------------- | -------------------- | --------------------------------- |
| ORICO CF1000 (Minisforum N5A, Alder Lake-N)  | `orico_cf1000_leds`  | ✅ v1.0                           |
| ORICO CF56Pro (Intel i5-1240P, Alder Lake-P) | `orico_cf56pro_leds` | 🔧 v1.0 (needs GPIO verification) |

---

## What it does

Installs a Linux kernel LED class driver that exposes every LED on the NAS
as a standard sysfs node under `/sys/class/leds/`. A companion daemon then
watches your drives and network and drives the LEDs automatically.

### Bay LEDs (per drive slot)

| State                               | LED                    |
| ----------------------------------- | ---------------------- |
| Drive present, healthy, in ZFS pool | Solid blue             |
| Drive present, SMART warning        | Slow red flash (1s)    |
| Drive present, SMART failure        | Fast red flash (250ms) |
| Drive expected in pool but missing  | Solid red              |
| Bay empty                           | Off                    |

### Network LED

Reflects aggregate traffic across all non-loopback interfaces via /proc/net/dev:

| State                 | LED    |
| --------------------- | ------ |
| Outbound traffic (TX) | Blue   |
| Inbound traffic (RX)  | Red    |
| Bidirectional         | Purple |
| Idle                  | Off    |

### System LED

Reflects aggregate disk I/O across all sd* and nvme* devices via /proc/diskstats:

| State       | LED    |
| ----------- | ------ |
| Disk reads  | Blue   |
| Disk writes | Red    |
| Both        | Purple |
| Idle        | Off    |

---

## How it works

The bay LEDs are wired directly to Intel PCH GPIO pads. The OEM firmware
controlled them via direct MMIO writes to PADCFG0 registers.

This project replaces that entirely:

- **orico_cf1000_leds.ko** / **orico_cf56pro_leds.ko** maps physical MMIO
  regions at module load time and registers led_classdev devices. It
  deliberately bypasses pinctrl-alderlake (which already owns these pads)
  via direct ioremap, the same mechanism the OEM used.

- **cf1000-ledd** / **cf56pro-ledd** is a pure userspace daemon. It reads
  /proc/diskstats, /proc/net/dev, zpool status, and smartctl, then writes
  brightness values to the sysfs nodes the driver exposes.

---

## Requirements

| Dependency                                     | Purpose                                  |
| ---------------------------------------------- | ---------------------------------------- |
| Linux kernel headers (matching running kernel) | Build the kernel module                  |
| gcc, make                                      | Build everything                         |
| zfsutils-linux                                 | ZFS pool membership detection (optional) |
| smartmontools                                  | SMART health monitoring (optional)       |

### Install kernel headers

| Distro          | Command                                    |
| --------------- | ------------------------------------------ |
| Ubuntu / Debian | sudo apt install linux-headers-$(uname -r) |

---

## CF1000 Installation (10-bay NAS)

```bash
make
sudo make install

# Auto-load module on boot
echo 'orico_cf1000_leds' | sudo tee /etc/modules-load.d/cf1000-leds.conf

# Enable and start the daemon
sudo make enable
```

---

## CF56Pro Installation (5-bay NAS)

> **Note:** The CF56Pro GPIO register offsets are initially sourced from the
> CF1000. You should run the probe tool after installation to verify each
> LED responds correctly. See the [Hardware Discovery Guide](#hardware-discovery-guide-cf56pro) below.

```bash
make cf56pro
sudo make install-cf56pro

# Auto-load module on boot
echo 'orico_cf56pro_leds' | sudo tee /etc/modules-load.d/cf56pro-leds.conf

# Enable and start the daemon
sudo make enable-cf56pro
```

### CF56Pro: Quick verification

After loading the module, run the probe tool to verify LEDs:

```bash
sudo ./cf56pro_led_test probe
```

This turns on each LED individually for 3 seconds. Watch your NAS and
note which physical LED lights up for each entry. If any LED doesn't
respond or lights up in the wrong location, see the discovery guide below.

---

## Configuration

### CF1000

Config: `/etc/cf1000-led.conf` (never overwritten by updates)
Defaults: `/usr/share/cf1000-led/cf1000-led.conf.default`

### CF56Pro

Config: `/etc/cf56pro-led.conf` (never overwritten by updates)
Defaults: `/usr/share/cf56pro-led/cf56pro-led.conf.default`

To reset to defaults:

```bash
# CF1000:
sudo cp /usr/share/cf1000-led/cf1000-led.conf.default /etc/cf1000-led.conf
sudo systemctl restart cf1000-ledd

# CF56Pro:
sudo cp /usr/share/cf56pro-led/cf56pro-led.conf.default /etc/cf56pro-led.conf
sudo systemctl restart cf56pro-ledd
```

---

## Operations

### After editing the config

```bash
# CF1000:
sudo systemctl restart cf1000-ledd

# CF56Pro:
sudo systemctl restart cf56pro-ledd
```

### Live logs

```bash
# CF1000:
journalctl -u cf1000-ledd -f

# CF56Pro:
journalctl -u cf56pro-ledd -f
```

### Status at a glance

```bash
make status
```

### Run in foreground (debugging)

```bash
# CF56Pro example:
sudo systemctl stop cf56pro-ledd
sudo cf56pro-ledd -f
# Ctrl-C exits cleanly and turns off all LEDs
sudo systemctl start cf56pro-ledd
```

### Manual LED control

```bash
# CF56Pro:
sudo ./cf56pro_led_test status
sudo ./cf56pro_led_test set 1 blue on
sudo ./cf56pro_led_test all both off
sudo ./cf56pro_led_test sweep
sudo ./cf56pro_led_test flash
sudo ./cf56pro_led_test probe
```

---

## Hardware Discovery Guide (CF56Pro)

The CF56Pro's GPIO register offsets and HCTL assignments may differ from
the CF1000. Follow these steps to verify and correct them.

### Step 1: Find your HCTL assignments

On your running Proxmox (or any Linux), discover which SCSI addresses
your drives use:

```bash
ls /sys/class/scsi_device/
```

This will show entries like `0:0:0:0`, `1:0:0:0`, etc. To map each HCTL
to a physical bay slot, insert a drive into each bay one at a time and
check which new HCTL appears.

Update the `g_bays[]` table in `cf56pro-ledd.c` with your correct HCTLs:

```c
static bay_t g_bays[NUM_BAYS] = {
    { .num = 1, .hctl = "X:0:0:0" },  /* replace with your bay 1 HCTL */
    { .num = 2, .hctl = "Y:0:0:0" },  /* replace with your bay 2 HCTL */
    { .num = 3, .hctl = "Z:0:0:0" },  /* etc. */
    { .num = 4, .hctl = "W:0:0:0" },
    { .num = 5, .hctl = "V:0:0:0" },
};
```

### Step 2: Verify GPIO register offsets

After loading the module (`sudo insmod orico_cf56pro_leds.ko`), run:

```bash
sudo ./cf56pro_led_test probe
```

Watch each LED as it lights up. If a bay's LED doesn't respond or lights
up at the wrong position, the GPIO register offset for that LED needs to
be corrected in `orico_cf56pro_leds.c`.

### Step 3: Check MMIO base addresses (if nothing works)

If no LEDs respond at all, the MMIO base addresses may be wrong. Check
your system's memory map:

```bash
cat /proc/iomem | grep -i gpio
```

Look for Intel PCH GPIO regions. On Alder Lake, you should see entries
near `0xFD6A0000` (Region A) and `0xFD6D0000` (Region D). If the
addresses differ, update `REGION_A_BASE` and `REGION_D_BASE` in
`orico_cf56pro_leds.c`.

### Step 4: Extract board-config (if available)

If you have access to the original CyberData OS image (or backed up the
partition), the GPIO register map lives at `/oem/board-config`. This file
lists all GPIO pad assignments including LED registers.

---

## Secure Boot

If insmod returns "Operation not permitted", Secure Boot is blocking the
unsigned module. Either disable Secure Boot in BIOS/UEFI, or sign the module
with your enrolled MOK key.

---

## Troubleshooting

**LEDs don't respond after insmod**
Check dmesg | grep cf56pro (or cf1000). Verify with lsmod | grep orico.

**Daemon starts but bay LEDs stay off**
Run: sudo cf56pro-ledd -f (or cf1000-ledd -f)
Watch the startup log for drive discovery and initial states.

**System LED flickering constantly**
ZFS background I/O is likely the cause. Raise io_threshold in
the config file and restart the daemon.

**Network LED flickering**
Raise net_threshold in the config file. Default is 64 bytes/tick.

---

## Contributing

Other ORICO NAS units using Intel PCH GPIO for LEDs should be supportable
with the PADCFG0 register map for your unit — this can be extracted
from the OEM firmware board config file.

Your contribution may help another!

---

## License

GPL v2. See LICENSE.
