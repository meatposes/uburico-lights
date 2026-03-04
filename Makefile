# Makefile - uburico-lights
#
# Top-level convenience targets that dispatch into hardware-specific
# subdirectories.  Each folder is self-contained and can be copied
# independently to the target NAS.
#
#   cf1000/   ORICO CF1000 (10-bay, Alder Lake-N)
#   cf56pro/  ORICO CF56Pro (5-bay, Alder Lake-P / i5-1240P)

.PHONY: cf1000 cf56pro clean

cf1000:
	$(MAKE) -C cf1000

cf56pro:
	$(MAKE) -C cf56pro

clean:
	-$(MAKE) -C cf1000 clean
	-$(MAKE) -C cf56pro clean
