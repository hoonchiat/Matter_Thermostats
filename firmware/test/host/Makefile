# Host-side unit tests for the pure components (control law, SHT40, occupancy,
# i18n, UI). These compile the pure sources natively — no ESP-IDF required.
#   make        # build + run all suites
#   make clean

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
LDLIBS  ?= -lm

CORE_INC  := -I../../components/thermostat_core/include
OCC_INC   := -I../../components/occupancy/include
SHT_INC   := -I../../components/sht4x/include
I18N_INC  := -I../../components/i18n/include
UI_INC    := -I../../components/ui_oled/include $(I18N_INC)
I18N_SRC  := ../../components/i18n/i18n.c

.PHONY: all test clean preview
all: test

test: test_thermostat_core test_occupancy test_sht4x test_i18n test_ui
	@echo "=== thermostat_core ==="; ./test_thermostat_core
	@echo "=== occupancy ===";       ./test_occupancy
	@echo "=== sht4x ===";           ./test_sht4x
	@echo "=== i18n ===";            ./test_i18n
	@echo "=== ui ===";              ./test_ui

# Render the OLED screens to the terminal as ASCII (no hardware needed).
preview: preview_ui
	./preview_ui

preview_ui: preview_ui.c ../../components/ui_oled/ui_oled.c ../../components/ui_oled/font5x7.inc $(I18N_SRC)
	$(CC) -DUI_OLED_HOST $(CFLAGS) $(UI_INC) preview_ui.c $(I18N_SRC) -o preview_ui $(LDLIBS)

test_thermostat_core: test_thermostat_core.c ../../components/thermostat_core/thermostat_core.c
	$(CC) $(CFLAGS) $(CORE_INC) $^ -o $@ $(LDLIBS)

test_occupancy: test_occupancy.c ../../components/occupancy/occupancy_logic.c
	$(CC) $(CFLAGS) $(OCC_INC) $^ -o $@ $(LDLIBS)

test_sht4x: test_sht4x.c ../../components/sht4x/sht4x_logic.c
	$(CC) $(CFLAGS) $(SHT_INC) $^ -o $@ $(LDLIBS)

test_i18n: test_i18n.c $(I18N_SRC)
	$(CC) $(CFLAGS) $(I18N_INC) $^ -o $@ $(LDLIBS)

test_ui: test_ui.c ../../components/ui_oled/ui_oled.c ../../components/ui_oled/font5x7.inc $(I18N_SRC)
	$(CC) -DUI_OLED_HOST $(CFLAGS) $(UI_INC) test_ui.c $(I18N_SRC) -o $@ $(LDLIBS)

# Emit every OLED screen's framebuffer as base64 JSON (feeds the user guide).
render_screens: render_screens.c ../../components/ui_oled/ui_oled.c ../../components/ui_oled/font5x7.inc $(I18N_SRC)
	$(CC) -DUI_OLED_HOST $(CFLAGS) $(UI_INC) render_screens.c $(I18N_SRC) -o $@ $(LDLIBS)

clean:
	rm -f test_thermostat_core test_occupancy test_sht4x test_i18n test_ui \
	      render_screens preview_ui
