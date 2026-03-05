# Makefile - uburico-lights / ORICO CF1000 (10-bay NAS)
#
# Targets:
#   make              build everything (module + daemon + test tool)
#   make module       kernel module only
#   make daemon       userspace daemon only
#   make test-tool    test CLI only
#   make clean        remove all build artifacts
#   make load         insmod the freshly built module
#   make unload       rmmod the module
#   make status       show module state, LED sysfs nodes, daemon state
#   make install      install module + daemon + config + systemd unit
#   make uninstall    remove installed files (leaves /etc/cf1000-led.conf)
#   make enable       systemd enable + start
#   make disable      systemd stop + disable
#   make restart      restart the daemon (apply config changes)
#   make logs         tail the daemon log

KDIR   ?= /lib/modules/$(shell uname -r)/build
KVER   ?= $(shell uname -r)
SRCDIR := $(CURDIR)

MODULE      := orico_cf1000_leds
DAEMON_SRC  := cf1000-ledd.c
DAEMON_BIN  := cf1000-ledd
TEST_SRC    := cf1000_led_test.c
TEST_BIN    := cf1000_led_test

CC          ?= gcc
CFLAGS      := -O2 -Wall -Wextra -std=c11 \
               -D_POSIX_C_SOURCE=200809L \
               -D_DEFAULT_SOURCE

INSTALL_SBIN    := /usr/local/sbin
INSTALL_SHARE   := /usr/share/cf1000-led
INSTALL_MODULES := /lib/modules/$(KVER)/extra
INSTALL_SYSTEMD := /etc/systemd/system
INSTALL_CONFIG  := /etc/cf1000-led.conf

.PHONY: all module daemon test-tool clean \
        load unload status \
        install uninstall enable disable restart logs

all: module daemon test-tool

module:
	$(MAKE) -C $(KDIR) M=$(SRCDIR) modules

daemon: $(DAEMON_BIN)
$(DAEMON_BIN): $(DAEMON_SRC)
	$(CC) $(CFLAGS) -o $@ $<

test-tool: $(TEST_BIN)
$(TEST_BIN): $(TEST_SRC)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(SRCDIR) clean
	rm -f $(DAEMON_BIN) $(TEST_BIN)

load: module
	@echo "Loading $(MODULE).ko ..."
	sudo insmod $(MODULE).ko
	@echo "Done. Check: dmesg | tail -20"

unload:
	@sudo rmmod $(MODULE) 2>/dev/null && echo "$(MODULE) unloaded" \
	  || echo "$(MODULE) was not loaded"

status:
	@echo "=== lsmod ==="
	@lsmod | grep cf1000 || echo "  (module not loaded)"
	@echo ""
	@echo "=== /sys/class/leds/cf1000* ==="
	@ls /sys/class/leds/ 2>/dev/null | grep cf1000 || echo "  (no cf1000 LEDs found)"
	@echo ""
	@echo "=== daemon ==="
	@systemctl is-active cf1000-ledd 2>/dev/null || echo "  (not managed by systemd)"
	@echo ""
	@echo "=== dmesg (last 15 lines) ==="
	@dmesg | tail -15

install: module daemon
	@echo "--- Installing kernel module ---"
	sudo mkdir -p $(INSTALL_MODULES)
	sudo install -m 644 $(MODULE).ko $(INSTALL_MODULES)/$(MODULE).ko
	sudo depmod -a $(KVER)
	@echo "--- Loading kernel module ---"
	-sudo modprobe $(MODULE)

	@echo "--- Installing daemon ---"
	sudo install -D -m 755 $(DAEMON_BIN) $(INSTALL_SBIN)/$(DAEMON_BIN)

	@echo "--- Installing default config ---"
	sudo install -d $(INSTALL_SHARE)
	sudo install -m 644 cf1000-led.conf $(INSTALL_SHARE)/cf1000-led.conf.default

	@if [ ! -f $(INSTALL_CONFIG) ]; then \
	    echo "--- Installing config to $(INSTALL_CONFIG) ---"; \
	    sudo install -m 644 cf1000-led.conf $(INSTALL_CONFIG); \
	else \
	    echo "--- $(INSTALL_CONFIG) already exists, not overwriting ---"; \
	    echo "    (default is at $(INSTALL_SHARE)/cf1000-led.conf.default)"; \
	fi

	@echo "--- Installing systemd unit ---"
	sudo install -m 644 cf1000-ledd.service $(INSTALL_SYSTEMD)/cf1000-ledd.service
	sudo systemctl daemon-reload

	@echo ""
	@echo "Installation complete."
	@echo ""
	@echo "Auto-load module on boot:"
	@echo "  echo '$(MODULE)' | sudo tee /etc/modules-load.d/cf1000-leds.conf"
	@echo ""
	@echo "Enable and start daemon:"
	@echo "  make enable"

uninstall:
	-sudo systemctl stop    cf1000-ledd 2>/dev/null
	-sudo systemctl disable cf1000-ledd 2>/dev/null
	sudo rm -f $(INSTALL_SYSTEMD)/cf1000-ledd.service
	sudo systemctl daemon-reload
	sudo rm -f $(INSTALL_SBIN)/$(DAEMON_BIN)
	sudo rm -f $(INSTALL_MODULES)/$(MODULE).ko
	sudo depmod -a $(KVER)
	sudo rm -rf $(INSTALL_SHARE)
	@echo ""
	@echo "Uninstalled. $(INSTALL_CONFIG) was left in place."
	@echo "Remove manually if desired: sudo rm $(INSTALL_CONFIG)"

enable:
	sudo systemctl enable --now cf1000-ledd
	@echo "Daemon enabled and started."
	@echo "Logs: journalctl -u cf1000-ledd -f"

disable:
	sudo systemctl stop    cf1000-ledd
	sudo systemctl disable cf1000-ledd
	@echo "Daemon stopped and disabled."

restart:
	sudo systemctl restart cf1000-ledd
	@echo "Daemon restarted. Logs: journalctl -u cf1000-ledd -f"

logs:
	journalctl -u cf1000-ledd -f
