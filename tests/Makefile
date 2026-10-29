# Host tests for the logic that does not need a Vita: the storage staging path,
# settings load/save, the formatter and the translations.
# Plain gcc, AddressSanitizer + UBSan; no VitaSDK needed.
#
#   make -C tests          build and run everything
#   make -C tests clean

CC      ?= gcc
CFLAGS  ?= -std=gnu99 -g -O1 -Wall -Wextra -Wno-unused-parameter \
           -fsanitize=address,undefined -fno-sanitize-recover=undefined \
           -fno-omit-frame-pointer
CFLAGS  += -Istubs

TESTS := test_stage test_settings test_gpumap test_hud test_util test_i18n test_bench test_fastcopy

.PHONY: all run clean
all: run

run: $(TESTS)
	@set -e; for t in $(TESTS); do ./$$t; done; echo "all host tests passed"

test_stage: test_stage.c fakes.c fakes.h vm_test.h ../kernel/stage.c ../kernel/stage.h
	$(CC) $(CFLAGS) -o $@ test_stage.c fakes.c

test_settings: test_settings.c fakes.c fakes.h vm_test.h ../kernel/settings.c ../kernel/settings.h
	$(CC) $(CFLAGS) -o $@ test_settings.c fakes.c

test_gpumap: test_gpumap.c fakes.c fakes.h vm_test.h ../kernel/gpumap.c ../kernel/gpumap.h
	$(CC) $(CFLAGS) -o $@ test_gpumap.c fakes.c

test_hud: test_hud.c fakes.c vm_test.h ../kernel/hud_draw.h ../kernel/hud.h ../kernel/font8x8.h ../include/vm_font_terminus.h
	$(CC) $(CFLAGS) -o $@ test_hud.c fakes.c

test_util: test_util.c vm_test.h fakes.c ../include/vm_fmt.h
	$(CC) $(CFLAGS) -o $@ test_util.c fakes.c

test_i18n: test_i18n.c vm_test.h fakes.c ../settings_app/vitamemio_i18n.c ../settings_app/vitamemio_i18n_table.h
	$(CC) $(CFLAGS) -o $@ test_i18n.c fakes.c

TESTER := ../tester/bench.c ../tester/report.c ../tester/results.c

test_bench: test_bench.c fakes.c fakes.h vm_test.h $(TESTER) ../tester/bench.h ../tester/pattern.h ../tester/report.h ../tester/results.h
	$(CC) $(CFLAGS) -o $@ test_bench.c $(TESTER) fakes.c

test_fastcopy: test_fastcopy.c vm_test.h ../kernel/fastcopy.h
	$(CC) $(CFLAGS) -o $@ test_fastcopy.c

# The fast copy's ARM (LDM/STM) path, run under qemu-user: needs
# gcc-arm-linux-gnueabihf and qemu-user. Thumb-2 (as VitaSDK builds) and ARM.
ARM_CC ?= arm-linux-gnueabihf-gcc
.PHONY: arm
arm: test_fastcopy.c vm_test.h ../kernel/fastcopy.h
	@set -e; for m in -mthumb -marm; do for o in -O1 -O2 -Os; do \
	  $(ARM_CC) -std=gnu99 $$m $$o -mcpu=cortex-a9 -Wall -Wextra -Werror -static \
	    -o test_fastcopy_arm test_fastcopy.c; \
	  printf '%s %s: ' $$m $$o; qemu-arm ./test_fastcopy_arm; \
	done; done

clean:
	rm -f test_fastcopy_arm $(TESTS) sim_app sim_main.o

# Syntax-only check of the console-only tester code against stub psp2 headers
# (the real build needs VitaSDK). Catches typos and wrong signatures; it cannot
# prove the code works on a Vita.
VITA_SRC := ../tester/main.c ../tester/vita/ui.c ../tester/vita/vita_env.c
VITA_STUBS := $(wildcard stubs_vita/psp2/*.h stubs_vita/psp2/*/*.h)

.PHONY: syntax-tester
syntax-tester: $(VITA_SRC) $(VITA_STUBS) ../tester/vita/ui.h ../tester/vita/vita_env.h
	for f in $(VITA_SRC); do \
	  $(CC) -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Werror -fsyntax-only -Istubs_vita $$f || exit 1; \
	done
	@echo "tester (console part): syntax ok"

# Run the real tester app on this PC against a fake console and write the screens
# out as images: make -C tests sim  (see sim/sim.c for the controls).
SIM_SRC := sim/sim.c ../tester/main.c ../tester/vita/ui.c ../tester/vita/vita_env.c \
           $(TESTER)

sim_app: $(SIM_SRC) $(VITA_STUBS) fakes.c
	$(CC) -std=gnu99 -g -O1 -Wall -Wno-unused-parameter -Istubs_vita -c -o sim_main.o sim/sim.c
	$(CC) -std=gnu99 -g -O1 -Wall -Wno-unused-parameter -Dmain=tester_main -Istubs_vita \
	  -o $@ sim_main.o ../tester/main.c ../tester/vita/ui.c ../tester/vita/vita_env.c \
	  $(TESTER)
