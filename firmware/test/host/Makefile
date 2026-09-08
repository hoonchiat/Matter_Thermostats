# Host-side unit tests for the pure components (thermistor conversion + control law).
# These compile the pure sources natively — no ESP-IDF required.
#   make        # build + run both suites
#   make clean

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
LDLIBS  ?= -lm

THERM_INC := -I../../components/thermistor/include
CORE_INC  := -I../../components/thermostat_core/include
OCC_INC   := -I../../components/occupancy/include
SHT_INC   := -I../../components/sht4x/include

.PHONY: all test clean preview
all: test

test: test_thermistor test_thermostat_core test_occupancy test_sht4x
	@echo "=== thermistor ===";      ./test_thermistor
	@echo "=== thermostat_core ==="; ./test_thermostat_core
	@echo "=== occupancy ===";       ./test_occupancy
	@echo "=== sht4x ===";           ./test_sht4x

# Render the OLED screens to the terminal as ASCII (no hardware needed).
preview: preview_ui
	./preview_ui

preview_ui: preview_ui.c ../../components/ui_oled/ui_oled.c ../../components/ui_oled/font5x7.inc
	$(CC) -DUI_OLED_HOST $(CFLAGS) -I../../components/ui_oled/include preview_ui.c -o preview_ui $(LDLIBS)

test_thermistor: test_thermistor.c ../../components/thermistor/ntc_curves.c
	$(CC) $(CFLAGS) $(THERM_INC) $^ -o $@ $(LDLIBS)

test_thermostat_core: test_thermostat_core.c ../../components/thermostat_core/thermostat_core.c
	$(CC) $(CFLAGS) $(CORE_INC) $^ -o $@ $(LDLIBS)

test_occupancy: test_occupancy.c ../../components/occupancy/occupancy_logic.c
	$(CC) $(CFLAGS) $(OCC_INC) $^ -o $@ $(LDLIBS)

test_sht4x: test_sht4x.c ../../components/sht4x/sht4x_logic.c
	$(CC) $(CFLAGS) $(SHT_INC) $^ -o $@ $(LDLIBS)

clean:
	rm -f test_thermistor test_thermostat_core test_occupancy test_sht4x preview_ui
