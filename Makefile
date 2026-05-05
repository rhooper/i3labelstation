# Top-level convenience wrapper around the PlatformIO project in
# esp32/labelscanstation/. All targets run via the local .venv.

VENV       := .venv
PIO        := $(VENV)/bin/platformio
PYTHON     := $(VENV)/bin/python
PIO_DIR    := esp32/labelscanstation
PORT       ?= /dev/tty.usbmodem5AE60209531

.PHONY: help setup build upload flash monitor clean fullclean

help:
	@echo "Label Scan Station — build targets"
	@echo ""
	@echo "  make setup       Create .venv and install platformio + pyserial"
	@echo "  make build       Compile firmware"
	@echo "  make upload      Compile and flash to $(PORT) (alias: flash)"
	@echo "  make monitor     Open serial monitor on $(PORT)"
	@echo "  make clean       Remove build artifacts"
	@echo "  make fullclean   Remove build artifacts + cached toolchains"
	@echo ""
	@echo "Override serial port: make upload PORT=/dev/tty.usbmodemXXXX"

$(PIO):
	@echo "platformio not found in $(VENV) — run 'make setup' first."
	@exit 1

setup:
	python3.14 -m venv $(VENV)
	$(VENV)/bin/pip install --upgrade pip
	$(VENV)/bin/pip install platformio pyserial

build: $(PIO)
	$(PIO) run -d $(PIO_DIR)

upload: $(PIO)
	$(PIO) run -d $(PIO_DIR) -t upload --upload-port $(PORT)

flash: upload

monitor: $(PIO)
	$(PIO) device monitor -d $(PIO_DIR) --port $(PORT)

clean: $(PIO)
	$(PIO) run -d $(PIO_DIR) -t clean

fullclean: $(PIO)
	$(PIO) run -d $(PIO_DIR) -t fullclean
