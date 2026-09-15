# Host-side unit tests for the pure components (control law, SHT40, occupancy, UI).
# These compile the pure sources natively — no ESP-IDF required.
#   make        # build + run both suites
#   make clean

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
LDLIBS  ?= -lm

CORE_INC  := -I../../components/thermostat_core/include
OCC_INC   := -I../../components/occupancy/include
SHT_INC   := -I../../components/sht4x/include

.PHONY: all test clean preview
all: test

test: test_thermostat_core test_occupancy test_sht4x test_ui
	@echo "=== thermostat_core ==="; ./test_thermostat_core
	@echo "=== occupancy ===";       ./test_occupancy
	@echo "=== sht4x ===";           ./test_sht4x
	@echo "=== ui ===";              ./test_ui

# Render the OLED screens to the terminal as ASCII (no hardware needed).
preview: preview_ui
	./preview_ui

preview_ui: preview_ui.c ../../components/ui_oled/ui_oled.c ../../components/ui_oled/font5x7.inc
	$(CC) -DUI_OLED_HOST $(CFLAGS) -I../../components/ui_oled/include preview_ui.c -o preview_ui $(LDLIBS)

test_thermostat_core: test_thermostat_core.c ../../components/thermostat_core/thermostat_core.c
	$(CC) $(CFLAGS) $(CORE_INC) $^ -o $@ $(LDLIBS)

test_occupancy: test_occupancy.c ../../components/occupancy/occupancy_logic.c
	$(CC) $(CFLAGS) $(OCC_INC) $^ -o $@ $(LDLIBS)

test_sht4x: test_sht4x.c ../../components/sht4x/sht4x_logic.c
	$(CC) $(CFLAGS) $(SHT_INC) $^ -o $@ $(LDLIBS)

test_ui: test_ui.c ../../components/ui_oled/ui_oled.c ../../components/ui_oled/font5x7.inc
	$(CC) -DUI_OLED_HOST $(CFLAGS) -I../../components/ui_oled/include test_ui.c -o $@ $(LDLIBS)

# Emit every OLED screen's framebuffer as base64 JSON (feeds the user guide).
render_screens: render_screens.c ../../components/ui_oled/ui_oled.c ../../components/ui_oled/font5x7.inc
	$(CC) -DUI_OLED_HOST $(CFLAGS) -I../../components/ui_oled/include render_screens.c -o $@ $(LDLIBS)

clean:
	rm -f test_thermostat_core test_occupancy test_sht4x test_ui \
	      render_screens preview_ui
