# vita-memio - build and deploy helpers.
#
#   make                      build both modules (uses your VitaSDK if $VITASDK
#                             is set, otherwise the pinned VitaSDK Docker image)
#   make install VITA_IP=...  upload the modules to ur0:tai/ over FTP
#   make deploy  VITA_IP=...  install, then reboot the Vita so they load
#   make log     VITA_IP=...  fetch the plugin logs into ./logs/
#   make config               print the lines to add to ur0:tai/config.txt
#   make test                 host tests (gcc + sanitizers, no VitaSDK)
#   make results VITA_IP=...  fetch results.txt written by the tester app into ./logs/
#   make cppcheck             static analysis
#   make clean
#
# install/deploy/log talk to vitacompanion (FTP on :1337, commands on :1338)
# running on the Vita. Output: build/vitamemio_kernel.skprx, build/vitamemio.suprx

BUILD   ?= build
VITA_IP ?=
# Extra CMake options passed to cmake
CMAKE_ARGS ?=
# Pinned so every build uses the same toolchain (VitaSDK 2026.08, GCC 15.2).
VITASDK_IMAGE ?= vitasdk/vitasdk@sha256:8227da1b596d981f4513cd9b36bda1e43857f6dae5bcf4312c1cd40167f7d9bb

MODULES := $(BUILD)/vitamemio_kernel.skprx $(BUILD)/vitamemio.suprx
TESTER_VPK := $(BUILD)/vitamemio_tester.vpk
FTP      = ftp://$(VITA_IP):1337

.PHONY: all build install deploy log results config clean check-ip test cppcheck sim

all: build

build:
ifdef VITASDK
	cmake -S . -B $(BUILD) -DCMAKE_TOOLCHAIN_FILE=$(VITASDK)/share/vita.toolchain.cmake $(CMAKE_ARGS)
	cmake --build $(BUILD) -j
else
	docker run --rm -v "$(CURDIR)":/src -w /src $(VITASDK_IMAGE) sh -c '\
	  cmake -S . -B $(BUILD) -DCMAKE_TOOLCHAIN_FILE=$$VITASDK/share/vita.toolchain.cmake $(CMAKE_ARGS) && \
	  cmake --build $(BUILD) -j$$(nproc) && \
	  chown -R $(shell id -u):$(shell id -g) $(BUILD)'
endif

check-ip:
	@test -n "$(VITA_IP)" || { echo "Set VITA_IP, e.g. make $(MAKECMDGOALS) VITA_IP=192.168.1.50"; exit 1; }

install: build check-ip
	@for f in $(MODULES); do \
	  echo "upload $$f"; curl -sS -T $$f "$(FTP)/ur0:/tai/$$(basename $$f)" || exit 1; \
	done
	@echo "Installed. If this is the first install, add the plugin to config.txt (make config)."

deploy: install
	@printf 'reboot\n' | nc -w 3 $(VITA_IP) 1338 >/dev/null || true
	@echo "Rebooting the Vita - the plugin loads at boot."

log: check-ip
	@mkdir -p logs
	curl -sS "$(FTP)/ur0:/data/vita-memio/kernel.log" -o logs/kernel.log
	curl -sS "$(FTP)/ur0:/data/vita-memio/user.log" -o logs/user.log
	@echo "Saved logs/kernel.log and logs/user.log"

results: check-ip
	@mkdir -p logs
	curl -sS "$(FTP)/ur0:/data/vita-memio/results.txt" -o logs/results.txt
	@echo "Saved logs/results.txt"

config:
	@printf '%s\n' '*KERNEL' 'ur0:tai/vitamemio_kernel.skprx' '' \
	  '*NPXS10015' 'ur0:tai/vitamemio.suprx'

# Host tests (no VitaSDK needed): see tests/Makefile.
test:
	$(MAKE) -C tests
	$(MAKE) -C tests syntax-tester

# Run the tester app on this PC against a fake console; writes screens as .ppm
# files into tests/sim_out/ (a quick look at the UI, not a Vita emulator).
sim:
	$(MAKE) -C tests sim_app
	mkdir -p tests/sim_out && cd tests/sim_out && SIM_SNAP_CHANGE=1 SIM_SCRIPT="10:cross" SIM_EXIT_FRAME=400 ../sim_app

cppcheck:
	cppcheck --enable=warning,performance,portability --inline-suppr --error-exitcode=1 --quiet \
	  -I include -I kernel -I user -I settings_app \
	  --suppress=missingIncludeSystem --suppress=unknownMacro \
	  kernel user tester settings_app/vitamemio_settings.c settings_app/vmlog.c settings_app/vitamemio_i18n.c

clean:
	rm -rf $(BUILD) logs
	$(MAKE) -C tests clean
