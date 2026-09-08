# Host-side unit tests for the pure components (thermistor conversion + control law).
# These compile the pure sources natively — no ESP-IDF required.
#   make        # build + run both suites
#   make clean

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
LDLIBS  ?= -lm

THERM_INC := -I../../components/thermistor/include
CORE_INC  := -I../../components/thermostat_core/include

.PHONY: all test clean
all: test

test: test_thermistor test_thermostat_core
	@echo "=== thermistor ===";      ./test_thermistor
	@echo "=== thermostat_core ==="; ./test_thermostat_core

test_thermistor: test_thermistor.c ../../components/thermistor/ntc_curves.c
	$(CC) $(CFLAGS) $(THERM_INC) $^ -o $@ $(LDLIBS)

test_thermostat_core: test_thermostat_core.c ../../components/thermostat_core/thermostat_core.c
	$(CC) $(CFLAGS) $(CORE_INC) $^ -o $@ $(LDLIBS)

clean:
	rm -f test_thermistor test_thermostat_core
