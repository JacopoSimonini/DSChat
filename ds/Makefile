# Host-side wrapper: builds the DS ROMs inside the official BlocksDS Docker
# image, so nothing needs installing on the host.
#
#   make                 -> dschat/dschat.nds and wifitest/dschat_wifitest.nds  (SSID DSCHAT, real DS)
#   make melon           -> the same for melonDS (SSID melonAP): *_melonAP.nds
#   make test            -> native unit tests of the app's pure-C modules (host gcc)
#   make APPS=dschat     -> only one app
#   make SSID=foo        -> default SSID (dschat: settings screen overrides it)
#   make APPS=wifitest PORT=1234 HOST=192.0.2.10 STATIC_IP=... STATIC_GW=... STATIC_MASK=...
#                        -> the wifitest diagnostic ROM has fixed build-time settings
#   make clean / make shell / make native (native BlocksDS install, $BLOCKSDS)

IMAGE   ?= skylyrac/blocksds:slim-latest
REPO    := $(abspath ..)
APPS    ?= dschat wifitest
DOCKER  = docker run --rm -u $(shell id -u):$(shell id -g) -e HOME=/tmp \
          -v $(REPO):/work -w /work/ds/$(1) $(IMAGE)
PASS    := $(foreach v,SSID PORT HOST MSG DEBUG STATIC_IP STATIC_GW STATIC_MASK,$(if $(value $(v)),$(v)='$($(v))'))

.PHONY: all melon test clean shell native
all:
	$(foreach a,$(APPS),$(call DOCKER,$(a)) make $(PASS) &&) true

melon:
	$(foreach a,$(APPS),$(call DOCKER,$(a)) make SSID=melonAP $(filter-out SSID=%,$(PASS)) &&) true

test:
	$(MAKE) -C dschat/tests

clean:
	$(foreach a,$(APPS),$(call DOCKER,$(a)) make clean &&) true
	rm -rf $(foreach a,$(APPS),$(a)/build $(a)/*.nds) dschat/tests/build

shell:
	docker run --rm -it -u $(shell id -u):$(shell id -g) -e HOME=/tmp \
	  -v $(REPO):/work -w /work/ds $(IMAGE) bash

native:
	$(foreach a,$(APPS),$(MAKE) -C $(a) $(PASS) &&) true
