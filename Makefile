# Makefile - uburico-lights
#
# Top-level convenience targets that dispatch into hardware-specific
# subdirectories.  Each folder is self-contained and can be copied
# independently to the target NAS.
#
#   cf1000/   ORICO CF1000 (10-bay, Alder Lake-N)
#   cf56pro/  ORICO CF56Pro (5-bay, Alder Lake-P / i5-1240P)

.PHONY: cf1000 cf56pro clean \
        install enable uninstall \
        install-cf56pro enable-cf56pro uninstall-cf56pro

cf1000:
	$(MAKE) -C cf1000

cf56pro:
	$(MAKE) -C cf56pro

# CF1000 is the default board for install/enable/uninstall.
install:
	$(MAKE) -C cf1000 install

enable:
	$(MAKE) -C cf1000 enable

uninstall:
	$(MAKE) -C cf1000 uninstall

install-cf56pro:
	$(MAKE) -C cf56pro install

enable-cf56pro:
	$(MAKE) -C cf56pro enable

uninstall-cf56pro:
	$(MAKE) -C cf56pro uninstall

clean:
	-$(MAKE) -C cf1000 clean
	-$(MAKE) -C cf56pro clean
