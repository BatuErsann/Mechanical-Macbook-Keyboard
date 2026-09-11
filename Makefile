APP_NAME := Haptik
BUILD_DIR := build
CORE_DIR := src/core
MACOS_DIR := src/macos
TOOLS_DIR := tools
INC_DIR := include
APP_BUNDLE := $(BUILD_DIR)/$(APP_NAME).app
APP_CONTENTS := $(APP_BUNDLE)/Contents
APP_MACOS := $(APP_CONTENTS)/MacOS
APP_RESOURCES := $(APP_CONTENTS)/Resources
SOUND_FILES := $(shell find resources/sounds -type f)
LOCALIZATION_FILES := $(shell find resources/macos -name 'Localizable.strings')

CC := xcrun clang
CFLAGS := -std=c11 -O2 -Wall -Wextra -Wpedantic -I$(INC_DIR) -Ithird_party
OBJCFLAGS := -fobjc-arc -O2 -Wall -Wextra -I$(INC_DIR)
FRAMEWORKS := -framework CoreFoundation -framework IOKit
AUDIO_FRAMEWORKS := -framework AudioToolbox -framework AudioUnit -framework CoreAudio
APP_FRAMEWORKS := $(FRAMEWORKS) $(AUDIO_FRAMEWORKS) -framework Cocoa -framework ApplicationServices

.PHONY: all app run probe clean

all: app

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/haptik_sensor.o: $(MACOS_DIR)/haptik_sensor.c $(INC_DIR)/haptik_sensor.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/sensor_probe.o: $(TOOLS_DIR)/sensor_probe.c $(INC_DIR)/haptik_sensor.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/haptik-sensor-probe: $(BUILD_DIR)/haptik_sensor.o $(BUILD_DIR)/sensor_probe.o
	$(CC) $^ -o $@ $(FRAMEWORKS)

probe: $(BUILD_DIR)/haptik-sensor-probe

$(BUILD_DIR)/haptik_impact.o: $(CORE_DIR)/haptik_impact.c $(INC_DIR)/haptik_impact.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/haptik_audio.o: $(CORE_DIR)/haptik_audio.c $(INC_DIR)/haptik_audio.h third_party/minimp3/minimp3.h third_party/minimp3/minimp3_ex.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/main.o: $(MACOS_DIR)/main.m $(INC_DIR)/haptik_sensor.h $(INC_DIR)/haptik_impact.h $(INC_DIR)/haptik_audio.h | $(BUILD_DIR)
	$(CC) $(OBJCFLAGS) -c $< -o $@

$(BUILD_DIR)/$(APP_NAME): $(BUILD_DIR)/main.o $(BUILD_DIR)/haptik_sensor.o $(BUILD_DIR)/haptik_impact.o $(BUILD_DIR)/haptik_audio.o
	$(CC) $^ -o $@ $(APP_FRAMEWORKS)

$(BUILD_DIR)/Haptik.icns: $(TOOLS_DIR)/make_icon.m | $(BUILD_DIR)
	$(CC) -fobjc-arc $< -framework Cocoa -o $(BUILD_DIR)/make-icon
	$(BUILD_DIR)/make-icon $@

$(APP_BUNDLE): $(BUILD_DIR)/$(APP_NAME) $(BUILD_DIR)/Haptik.icns resources/macos/Info.plist $(SOUND_FILES) $(LOCALIZATION_FILES)
	mkdir -p $(APP_MACOS) $(APP_RESOURCES)/Sounds/KailhWhite $(APP_RESOURCES)/Sounds/KBSim
	cp $(BUILD_DIR)/Haptik.icns $(APP_RESOURCES)/Haptik.icns
	cp resources/macos/Info.plist $(APP_CONTENTS)/Info.plist
	cp -R resources/macos/*.lproj $(APP_RESOURCES)/
	cp $(BUILD_DIR)/$(APP_NAME) $(APP_MACOS)/$(APP_NAME)
	cp resources/sounds/kailh_white/* $(APP_RESOURCES)/Sounds/KailhWhite/
	cp -R resources/sounds/kbsim/. $(APP_RESOURCES)/Sounds/KBSim/
	codesign --force --sign - --timestamp=none \
		--requirements '=designated => identifier "com.barty.haptik"' \
		$(APP_BUNDLE)
	touch $(APP_BUNDLE)

app: $(APP_BUNDLE)

run: app
	open $(APP_BUNDLE)

clean:
	rm -rf $(BUILD_DIR)
