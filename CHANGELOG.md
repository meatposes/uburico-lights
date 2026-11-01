# Changelog

All notable changes to uburico-lights will be documented here.

## [Unreleased]

### Fixed
- The kernel module no longer disappears after a kernel upgrade. `make install`
  now registers it with DKMS (`dkms.conf` per board), so it is rebuilt for each
  new kernel. Without DKMS it falls back to the old one-kernel install and warns.
- The daemon unit runs `modprobe` before waiting for the LED sysfs nodes, and
  stops after 5 failed starts in 5 minutes instead of restarting every 10 s
  forever. The failure message now points at `dkms status`.
- Top-level `make install`, `make enable`, `make install-cf56pro` and
  `make enable-cf56pro` now exist, as the README describes.
- Unit `Documentation=` points at the real repository.

## [1.0.0] - 2026-03-04

Initial public release. Supports ORICO CF1000 (Minisforum N5A, Intel Alder Lake-N).

### Kernel module (orico_cf1000_leds.ko)
- Maps two PCH GPIO MMIO regions (0xFD6A0000, 0xFD6D0000) via ioremap
- Registers 24 LED class devices: 10 bays x blue/red, plus network and system indicators
- Correct polarity handling for inverted pads (GPP_H4, GPP_H5, GPP_H7, GPP_F13)
- All LEDs driven off on module unload
- Out-of-tree Kbuild compatible with Ubuntu/Debian/Fedora/Arch/openSUSE

### Daemon (cf1000-ledd)
- Drive bay LEDs: solid blue (healthy), slow red flash (SMART warning),
  fast red flash (SMART fail), solid red (missing from ZFS pool), off (empty)
- SMART monitoring via smartctl with standby-respect (-n standby) to avoid
  spinning up idle drives
- ZFS pool membership via zpool status -vPL
- Hot-swap detection via /sys/class/scsi_device HCTL glob
- Network LED: blue=TX, red=RX, purple=both, off=idle (all non-loopback interfaces)
- System LED: blue=reads, red=writes, purple=both, off=idle (all sd*/nvme* devices)
- Configurable thresholds, poll intervals, and SMART check frequency
- systemd Type=simple with clean LED shutdown on SIGTERM
- Foreground mode (-f) for debugging
- Config protected from overwrites on update

### Test utility (cf1000_led_test)
- status, set, all commands for manual LED control
- Knight Rider sweep (blue, red, purple passes)
- Split-speed flash test (validates daemon flash timing)
- Basic on/off cycle test
- Clean Ctrl-C handling in all animation modes

### Build system
- Single Makefile: make, make install, make enable, make restart, make logs
- Config installed to /etc/cf1000-led.conf only if not already present
- Upstream defaults preserved at /usr/share/cf1000-led/cf1000-led.conf.default
