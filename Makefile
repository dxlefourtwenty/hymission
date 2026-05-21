SHELL := /usr/bin/bash
.SHELLFLAGS := -euo pipefail -c

BUILD_DIR ?= build-cmake
BUILD_TYPE ?= Release
JOBS ?= $(shell nproc)
DRY_RUN ?= 1

.PHONY: all configure build test clean hyprpm-update hyprpm-reload hyprpm-refresh status

all: build

configure:
	cmake -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -B $(BUILD_DIR)

build: configure
	cmake --build $(BUILD_DIR) -j $(JOBS)

test: build
	./$(BUILD_DIR)/hymission-mission-layout-test
	./$(BUILD_DIR)/hymission-overview-logic-test

clean:
	cmake --build $(BUILD_DIR) --target clean

hyprpm-update:
	@if [ "$(DRY_RUN)" = "1" ]; then \
		printf '%s\n' 'DRY_RUN=1: hyprpm update'; \
	else \
		hyprpm update; \
	fi

hyprpm-reload:
	@if [ "$(DRY_RUN)" = "1" ]; then \
		printf '%s\n' 'DRY_RUN=1: hyprpm reload'; \
	else \
		hyprpm reload; \
	fi

hyprpm-refresh: hyprpm-update hyprpm-reload

status:
	git status --short
