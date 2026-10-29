# Host tests for the logic that does not need a Vita: the storage staging path,
# settings load/save, the command-port parser, the formatter and the translations.
# Plain gcc, AddressSanitizer + UBSan; no VitaSDK needed.
#
#   make -C tests          build and run everything
#   make -C tests clean

CC      ?= gcc
CFLAGS  ?= -std=gnu99 -g -O1 -Wall -Wextra -Wno-unused-parameter \
           -fsanitize=address,undefined -fno-sanitize-recover=undefined \
           -fno-omit-frame-pointer
CFLAGS  += -Istubs

TESTS := test_stage test_settings test_dispatch test_dispatch_risky test_util test_i18n test_bench test_restore

.PHONY: all run clean
all: run

run: $(TESTS)
	@set -e; for t in $(TESTS); do ./$$t; done; echo "all host tests passed"

test_stage: test_stage.c fakes.c fakes.h ts_test.h ../kernel/stage.c ../kernel/stage.h
	$(CC) $(CFLAGS) -o $@ test_stage.c fakes.c

test_settings: test_settings.c fakes.c fakes.h ts_test.h ../kernel/settings.c ../kernel/settings.h
	$(CC) $(CFLAGS) -o $@ test_settings.c fakes.c

test_dispatch: test_dispatch.c fakes.c fakes.h ts_test.h ../user/dispatch.c
	$(CC) $(CFLAGS) -o $@ test_dispatch.c fakes.c

test_dispatch_risky: test_dispatch.c fakes.c fakes.h ts_test.h ../user/dispatch.c
	$(CC) $(CFLAGS) -DTS_DEV_ALLOW_RISKY=1 -o $@ test_dispatch.c fakes.c

test_util: test_util.c ts_test.h fakes.c ../include/ts_fmt.h ../include/ts_devmode.h
	$(CC) $(CFLAGS) -o $@ test_util.c fakes.c

test_i18n: test_i18n.c ts_test.h fakes.c ../settings_app/storageturbo_i18n.c ../settings_app/storageturbo_i18n_table.h
	$(CC) $(CFLAGS) -o $@ test_i18n.c fakes.c

TESTER := ../tester/bench.c ../tester/report.c ../tester/results.c

test_bench: test_bench.c fakes.c fakes.h ts_test.h $(TESTER) ../tester/bench.h ../tester/pattern.h ../tester/report.h ../tester/results.h
	$(CC) $(CFLAGS) -o $@ test_bench.c $(TESTER) fakes.c

test_restore: test_restore.c ts_test.h ../tester/restore.c ../tester/restore.h
	$(CC) $(CFLAGS) -o $@ test_restore.c

clean:
	rm -f $(TESTS) sim_app sim_main.o

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
           $(TESTER) ../tester/restore.c

sim_app: $(SIM_SRC) $(VITA_STUBS) fakes.c
	$(CC) -std=gnu99 -g -O1 -Wall -Wno-unused-parameter -Istubs_vita -c -o sim_main.o sim/sim.c
	$(CC) -std=gnu99 -g -O1 -Wall -Wno-unused-parameter -Dmain=tester_main -Istubs_vita \
	  -o $@ sim_main.o ../tester/main.c ../tester/vita/ui.c ../tester/vita/vita_env.c \
	  $(TESTER) ../tester/restore.c
