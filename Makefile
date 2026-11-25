# Project setup and configuration commands.

.DEFAULT_GOAL := help

PYTHON := $(shell command -v uv >/dev/null 2>&1 && echo "uv run python" || echo "python3")
# Stamped when make starts so the total build time also covers prerequisite targets.
MAKE_STARTED := $(shell date +%s)
NEWMOTA = NEWMOTA_MAKE_STARTED=$(MAKE_STARTED) PYTHONPATH="$(CURDIR)/tools" $(PYTHON) -m newmota
UV := $(shell command -v uv >/dev/null 2>&1 && echo "uv" || echo "")
HOST_CXX ?= c++
HOST_CC ?= cc
ifneq ($(strip $(SDK_LOG_LEVEL)),)
export SDK_LOG_LEVEL
endif
ifneq ($(strip $(VERBOSE)),)
export NEWMOTA_VERBOSE := $(VERBOSE)
endif
ENV_FILES := $(wildcard config/env/*/*/*.yaml user_config/*.yaml)
ENV_TARGETS := $(basename $(notdir $(ENV_FILES)))
BOARD_FILES := $(wildcard config/boards/*/*.json)
BOARD_TARGETS := $(basename $(notdir $(BOARD_FILES)))
BOARD_ONLY_TARGETS := $(filter-out $(ENV_TARGETS),$(BOARD_TARGETS))
# Recursive assignment: the Python query only runs for the crypto host targets.
# newmota.crypto owns the selected sources and the defines they need, so this
# Makefile never repeats them.
BEARSSL_HOST = $(shell PYTHONPATH=tools $(PYTHON) -c 'from newmota.crypto import host_build_inputs; print(" ".join(host_build_inputs()))')
BEARSSL_ROOT = src/third_party/bearssl
BEARSSL_SRC = $(filter %.c,$(BEARSSL_HOST))
BEARSSL_OBJ = $(patsubst src/%.c,build/host/crypto/%.o,$(BEARSSL_SRC))
# Vendored sources keep -Wall -Wextra without the project's -Wconversion/-Werror
# pair, which rejects BearSSL's fixed-width arithmetic.
BEARSSL_CFLAGS = -std=c11 -Wall -Wextra $(addprefix -D,$(filter %=1,$(BEARSSL_HOST)))
# The pinned supplicant's SAE code, built on the host against the BearSSL hostap backend.
# The path comes from the SDK resolver (extracted archive or .sdk/ checkout), queried once
# and only by the targets that use it.
HOSTAP_QUERY = $(eval HOSTAP_QUERY := $(shell PYTHONPATH=tools $(PYTHON) -c 'from newmota.host import load_sdk_version; s = load_sdk_version("esp-idf"); print(s.source / "components", s.mode)'))$(HOSTAP_QUERY)
HOSTAP_IDF = $(word 1,$(HOSTAP_QUERY))
HOSTAP_MODE = $(word 2,$(HOSTAP_QUERY))
HOSTAP = $(HOSTAP_IDF)/wpa_supplicant
HOSTAP_SRC = $(addprefix $(HOSTAP)/src/,common/sae.c common/dragonfly.c utils/common.c \
	utils/wpabuf.c utils/wpa_debug.c crypto/sha256.c crypto/sha256-kdf.c crypto/sha256-prf.c \
	crypto/sha1.c crypto/sha1-pbkdf2.c crypto/md5.c crypto/aes-wrap.c crypto/aes-unwrap.c \
	crypto/aes-omac1.c crypto/aes-siv.c crypto/aes-ctr.c crypto/dh_groups.c)
HOSTAP_BACKEND := src/platform/crypto/hostap_sae_crypto.c src/platform/crypto/hostap_symmetric_bearssl.c
HOSTAP_SDK_INC = $(addprefix $(HOSTAP)/,src src/utils src/crypto port/include include \
	esp_supplicant/include esp_supplicant/src) $(addprefix $(HOSTAP_IDF)/,esp_wifi/include \
	esp_wifi/include/local esp_event/include esp_netif/include \
	esp_wifi/wifi_apps/roaming_app/include esp_hw_support/include)
HOSTAP_DEFS := -std=gnu11 -fsanitize=address,undefined -DESP_PLATFORM -DESP_SUPPLICANT \
	-DCONFIG_WPA3_SAE -DCONFIG_SAE_H2E -DCONFIG_SAE -DCONFIG_ECC -DCONFIG_SHA256 \
	-DCONFIG_CRYPTO_INTERNAL -DCONFIG_NO_STDOUT_DEBUG -DBR_LOMUL=1
HOSTAP_CFLAGS = $(HOSTAP_DEFS) -Itools/tests/crypto/hostap $(addprefix -I,$(HOSTAP_SDK_INC)) \
	-I$(BEARSSL_ROOT)/src -I$(BEARSSL_ROOT)/extensions
CRYPTO_KAT_HDR := tools/tests/crypto/crypto_kat.h
CRYPTO_KAT_SRC := tools/tests/crypto/crypto_kat_main.cpp \
	tools/tests/crypto/crypto_hash_kat.cpp \
	tools/tests/crypto/crypto_sym_kat.cpp \
	tools/tests/crypto/crypto_ec_kat.cpp \
	tools/tests/crypto/crypto_extension_kat.cpp

ifneq ($(strip $(SDK_PYTHON)),)
export NEWMOTA_SETUP_PYTHON := $(SDK_PYTHON)
endif

.PHONY: setup sdk-check sdk-checkout sdk-fetch uv-setup list config validate-catalog build test test-host test-crypto all clangd format-cpp format-check-cpp probe backup flash flash-monitor update flash-only flash-only-monitor monitor clean help $(ENV_TARGETS) $(BOARD_ONLY_TARGETS)

setup:
	@$(NEWMOTA) setup \
		$(if $(PLATFORM),--platform $(PLATFORM),) \
		$(if $(ENV),--env $(ENV),) \
		$(if $(DRY_RUN),--dry-run,)

sdk-check:
	@$(NEWMOTA) sdk-check $(SDK)

sdk-checkout:
	@test -n "$(SDK)" || { echo "usage: make sdk-checkout SDK=<esp-idf|esp-idf-v6.1|bouffalo|beken|amebad|pico-sdk>"; exit 2; }
	@$(NEWMOTA) sdk-checkout $(SDK) $(if $(DRY_RUN),--dry-run,)

sdk-fetch:
	@$(NEWMOTA) sdk-fetch $(SDK) $(if $(DRY_RUN),--dry-run,)

uv-setup:
	@if [ -n "$(UV)" ]; then uv sync --locked; else echo "uv not found — install it from https://docs.astral.sh/uv/"; exit 1; fi

list:
	@$(NEWMOTA) list

config:
	@$(PYTHON) tools/menuconfig.py $(if $(ENV),--env $(ENV),)

validate-catalog:
	@$(NEWMOTA) validate

build:
	@$(NEWMOTA) build $(if $(BOARD),--board $(BOARD),$(if $(ENV),$(ENV),--all)) $(if $(JOBS),--jobs $(JOBS),)

test: validate-catalog test-host
	@$(NEWMOTA) build --matrix test $(if $(JOBS),--jobs $(JOBS),)

# Host tests cover only pure logic whose failure could brick a device or corrupt
# persistent data without a visible runtime symptom: flash/image/OTA bounds, settings
# layout, update sessions, and MQTT wire parsing. The native matrix covers the rest.
COMMA := ,
HOST_TEST_FLAGS := -std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -Wconversion -Werror
HOST_APP_INC := -Isrc/app -Isrc/app/include -Isrc/platform/include
COREMQTT := src/third_party/coremqtt/v2.3.1
host_test = $(HOST_CXX) $(HOST_TEST_FLAGS) $(2) -o build/host/$(1)-test && build/host/$(1)-test

test-host:
	@mkdir -p build/host
	@$(call host_test,energy-storage,-fsanitize=address$(COMMA)undefined -fno-omit-frame-pointer $(HOST_APP_INC) src/app/services/energy/energy_storage.cpp src/app/services/energy/energy_types.cpp src/app/services/energy/energy_accounting.cpp src/app/support/text.cpp src/app/support/crc32_portable.cpp tools/tests/energy_storage.cpp)
	@$(call host_test,ntp-packet,-Isrc/app -Isrc/platform/include src/platform/ntp_packet.cpp tools/tests/ntp_packet.cpp)
	@$(call host_test,rtc-calendar,-Isrc/app tools/tests/rtc_calendar.cpp)
	@$(call host_test,ubx-time,-Isrc/app src/app/drivers/gps/ubx_protocol.cpp tools/tests/ubx_time.cpp)
	@$(call host_test,esp-tasmota-settings,-Isrc/app tools/tests/esp_tasmota_settings.cpp src/app/services/migration/tasmota_settings.cpp)
	@$(call host_test,url-update-limits,-Isrc/app tools/tests/url_update_limits.cpp)
	@$(call host_test,update-session,$(HOST_APP_INC) src/app/core/update/update_session.cpp tools/tests/update_session.cpp)
	@$(call host_test,pico-picobin-image,-Isrc/platform/pico-sdk tools/tests/pico_picobin_image.cpp)
	@$(call host_test,pico-rp2040-image,-Isrc/platform/pico-sdk tools/tests/pico_rp2040_image.cpp)
	@$(call host_test,flash-capacity,-Isrc/platform/include tools/tests/flash_capacity.cpp)
	@$(call host_test,amebad-image-size,-fsanitize=address$(COMMA)undefined -fno-omit-frame-pointer -Isrc/platform/amebad tools/tests/amebad_image_size.cpp)
	@$(call host_test,pico-ble-bond,-fsanitize=address$(COMMA)undefined -fno-omit-frame-pointer -Isrc/platform/pico-sdk tools/tests/pico_ble_bond.cpp)
	@$(call host_test,amebad-ble-bond,-fsanitize=address$(COMMA)undefined -fno-omit-frame-pointer -Isrc/platform/amebad tools/tests/amebad_ble_bond.cpp)
	@$(call host_test,matter-wire,-fsanitize=address$(COMMA)undefined -fno-omit-frame-pointer -Isrc/app src/app/services/matter/matter_wire.cpp src/app/services/matter/matter_dns_sd.cpp src/app/services/matter/matter_btp.cpp src/app/services/matter/matter_cert.cpp tools/tests/matter_wire.cpp)
	@$(call host_test,critical-settings,-DNEWMOTA_CRITICAL_SETTINGS_STORE=1 $(HOST_APP_INC) -Itools/tests/hardware src/app/core/hardware/hardware.cpp src/app/services/settings/settings.cpp src/app/services/mqtt/mqtt_settings.cpp src/app/support/crc32_portable.cpp tools/tests/critical_settings.cpp)
	@$(call host_test,boot-counter,$(HOST_APP_INC) -Itools/tests/hardware src/app/core/system/boot_counter.cpp src/app/support/crc32_portable.cpp tools/tests/boot_counter.cpp)
	@$(call host_test,mqtt-wire,$(HOST_APP_INC) src/app/services/mqtt/mqtt_wire.cpp tools/tests/mqtt_wire.cpp)
	@$(HOST_CC) -std=c11 -DMQTT_DO_NOT_USE_CUSTOM_CONFIG=1 -Wall -Wextra -Wconversion -Werror \
		-I$(COREMQTT)/include -I$(COREMQTT)/interface \
		-c $(COREMQTT)/core_mqtt_serializer.c -o build/host/core-mqtt-serializer.o
	@$(call host_test,mqtt-codec,$(HOST_APP_INC) -I$(COREMQTT)/include -I$(COREMQTT)/interface -Itools/tests/mqtt src/app/services/mqtt/mqtt_wire.cpp src/app/services/mqtt/mqtt_codec.cpp tools/tests/mqtt_codec.cpp build/host/core-mqtt-serializer.o)
	@PYTHONPATH=tools $(PYTHON) -m unittest discover -s tools/tests -p 'test_*.py'

# Opt-in BearSSL checks; run after changing the crypto import or its extensions.
test-crypto:
	@mkdir -p build/host
	@$(MAKE) --no-print-directory build/host/crypto-primitives-test
	@build/host/crypto-primitives-test
	@$(HOST_CXX) -std=c++17 -Wall -Wextra -Wconversion -Werror \
		-Isrc/platform/include -Itools/tests/crypto/esp-idf \
		tools/tests/crypto/crypto_random_test.cpp src/platform/esp-idf/crypto_random.cpp \
		-o build/host/crypto-random-test
	@build/host/crypto-random-test
	@if [ -f $(HOSTAP)/src/common/sae.c ]; then \
		$(MAKE) --no-print-directory build/host/hostap-sae-test build/host/hostap-bignum-fuzz && \
		build/host/hostap-sae-test 200; \
	elif [ "$(HOSTAP_MODE)" = archive ]; then \
		echo "hostap SAE test: $(HOSTAP) is missing from the ESP-IDF archive" >&2; exit 1; \
	else echo "hostap SAE test: skipped (no $(HOSTAP))"; fi
	@PYTHONPATH=tools $(PYTHON) -m unittest discover -s tools/tests/crypto -p 'test_*.py'

# Rebuild this small host baseline each invocation so headers, compiler flags,
# and source selection/removal cannot leave stale objects or a stale executable.
.PHONY: force-crypto-host
force-crypto-host:

build/host/crypto/%.o: src/%.c force-crypto-host
	@mkdir -p $(dir $@)
	@$(HOST_CC) $(BEARSSL_CFLAGS) -I $(BEARSSL_ROOT)/src -Isrc/third_party/bearssl/extensions -c $< -o $@

# Vendored hostap sources build with -w; the backend keeps the project's warning set and is
# compiled once for both drivers.
HOSTAP_BACKEND_OBJ := $(patsubst src/platform/crypto/%.c,build/host/hostap/%.o,$(HOSTAP_BACKEND))

build/host/hostap/%.o: src/platform/crypto/%.c force-crypto-host
	@mkdir -p $(dir $@)
	@$(HOST_CC) $(HOSTAP_DEFS) -Wall -Wextra -Wconversion -Werror -isystem tools/tests/crypto/hostap \
		$(addprefix -isystem ,$(HOSTAP_SDK_INC)) -I$(BEARSSL_ROOT)/src -I$(BEARSSL_ROOT)/extensions \
		-c $< -o $@

build/host/hostap-sae-test: tools/tests/crypto/hostap_sae_test.c $(HOSTAP_BACKEND_OBJ) $(BEARSSL_OBJ) force-crypto-host
	@$(HOST_CC) $(HOSTAP_CFLAGS) -w $(HOSTAP_SRC) $< $(HOSTAP_BACKEND_OBJ) $(BEARSSL_OBJ) -o $@

build/host/hostap-bignum-fuzz: tools/tests/crypto/hostap_bignum_fuzz.c build/host/hostap/hostap_sae_crypto.o $(BEARSSL_OBJ) force-crypto-host
	@$(HOST_CC) $(HOSTAP_CFLAGS) -w $< $(HOSTAP)/src/utils/wpabuf.c build/host/hostap/hostap_sae_crypto.o \
		$(BEARSSL_OBJ) -o $@

build/host/crypto-primitives-test: $(BEARSSL_OBJ) $(CRYPTO_KAT_SRC) $(CRYPTO_KAT_HDR) force-crypto-host
	@mkdir -p $(dir $@)
	@$(HOST_CXX) -std=c++17 -fno-exceptions -fno-rtti \
		-Wall -Wextra -Wconversion -Werror \
		-I $(BEARSSL_ROOT)/src -Isrc/third_party/bearssl/extensions -Itools/tests/crypto \
		$(CRYPTO_KAT_SRC) \
		$(BEARSSL_OBJ) \
		-o $@

all:
	@$(NEWMOTA) build --all $(if $(JOBS),--jobs $(JOBS),)

clangd:
	@test -n "$(ENV)" || { echo "ENV is required: make clangd ENV=<environment>"; exit 2; }
	@$(NEWMOTA) clangd "$(ENV)" $(if $(JOBS),--jobs $(JOBS),)

format-cpp:
	@test -n "$(UV)" || { echo "uv is required; see https://docs.astral.sh/uv/"; exit 2; }
	@$(UV) run --locked --extra dev python tools/format_cpp.py --write

format-check-cpp:
	@test -n "$(UV)" || { echo "uv is required; see https://docs.astral.sh/uv/"; exit 2; }
	@$(UV) run --locked --extra dev python tools/format_cpp.py --check

probe:
	@test -n "$(ENV)" || { echo "ENV is required: make probe ENV=<environment> [PORT=<port>]"; exit 2; }
	@$(NEWMOTA) probe "$(ENV)" $(if $(PORT),--port "$(PORT)",) $(if $(BAUD),--baud $(BAUD),)

backup:
	@test -n "$(ENV)" || { echo "ENV is required: make backup ENV=<environment> OUTPUT=<file> [PORT=<port>]"; exit 2; }
	@test -n "$(OUTPUT)" || { echo "OUTPUT is required: make backup ENV=<environment> OUTPUT=<file> [PORT=<port>]"; exit 2; }
	@$(NEWMOTA) backup "$(ENV)" --output "$(OUTPUT)" $(if $(PORT),--port "$(PORT)",) $(if $(BAUD),--baud $(BAUD),)

flash flash-monitor:
	@test -n "$(ENV)" || { echo "ENV is required: make $@ ENV=<environment> [PORT=<port>]"; exit 2; }
	@$(NEWMOTA) $@ "$(ENV)" $(if $(PORT),--port "$(PORT)",) $(if $(BAUD),--baud $(BAUD),) $(if $(MONITOR_PORT),--monitor-port "$(MONITOR_PORT)",) $(if $(MONITOR_BAUD),--monitor-baud $(MONITOR_BAUD),) $(if $(JOBS),--jobs $(JOBS),)

update:
	@test -n "$(ENV)" || { echo "ENV is required: make update ENV=<environment> [PORT=<port>]"; exit 2; }
	@$(NEWMOTA) update "$(ENV)" $(if $(PORT),--port "$(PORT)",) $(if $(BAUD),--baud $(BAUD),) $(if $(JOBS),--jobs $(JOBS),)

flash-only flash-only-monitor:
	@test -n "$(PACKAGE)" || { echo "PACKAGE is required: make $@ PACKAGE=<manifest.json|factory.zip> [PORT=<port>]"; exit 2; }
	@$(NEWMOTA) $@ --package "$(PACKAGE)" $(if $(PORT),--port "$(PORT)",) $(if $(BAUD),--baud $(BAUD),) $(if $(MONITOR_PORT),--monitor-port "$(MONITOR_PORT)",) $(if $(MONITOR_BAUD),--monitor-baud $(MONITOR_BAUD),)

monitor:
	@test -n "$(ENV)" || { echo "ENV is required: make monitor ENV=<environment> [PORT=<port>]"; exit 2; }
	@$(NEWMOTA) monitor "$(ENV)" $(if $(PORT),--port "$(PORT)",) $(if $(BAUD),--baud $(BAUD),)

$(ENV_TARGETS):
	@$(NEWMOTA) build $@ $(if $(JOBS),--jobs $(JOBS),)

$(BOARD_ONLY_TARGETS):
	@$(NEWMOTA) build --board $@ $(if $(JOBS),--jobs $(JOBS),)

clean:
	@rm -rf build
	@rm -rf .cache/clangd
	@rm -f compile_commands.json compile_commands.json.tmp
	@echo "Cleaned build/"

help:
	@echo "Newmota project commands"
	@echo "  make uv-setup                      # create/update the project Python environment"
	@echo "  make setup PLATFORM=esp            # install ESP-IDF tools for all ESP envs"
	@echo "  make setup PLATFORM=bouffalo       # install Bouffalo SDK and toolchain"
	@echo "  make setup PLATFORM=beken          # install Beken SDK and toolchain"
	@echo "  make setup PLATFORM=amebad         # install Realtek AmebaD SDK and toolchain"
	@echo "  make setup PLATFORM=pico           # install Pico SDK, Arm toolchain, and picotool"
	@echo "  make setup PLATFORM=all            # set up all five platforms"
	@echo "  make setup ENV=esp32c2-2m          # install only the tools needed by one env"
	@echo "  make setup ... SDK_PYTHON=/path    # override the host Python used by SDK installers"
	@echo "  make sdk-check                     # verify installed SDKs against config/sdk/*.toml"
	@echo "  make sdk-fetch                     # download every pinned SDK archive (offline use)"
	@echo "  make sdk-checkout SDK=bouffalo     # git checkout in .sdk/ for SDK patch work"
	@echo "  make list                          # list supported and personal environments"
	@echo "  make config                        # browse checked-in chips, boards, and environments"
	@echo "  make validate-catalog              # validate supported and personal config"
	@echo "  make <environment-or-board>        # lazily set up and build one target"
	@echo "  make build ENV=<environment>       # equivalent explicit form"
	@echo "  make build ENV=bw16-2m VERBOSE=1  # show native SDK build commands"
	@echo "  make build SDK_LOG_LEVEL=none      # omit SDK logging for size measurement"
	@echo "  make build BOARD=<board>           # build any catalog board on demand"
	@echo "  make test                          # build the fast checked-in test matrix"
	@echo "  make test-host                     # run the small host-side safety tests"
	@echo "  make test-crypto                   # run opt-in BearSSL checks"
	@echo "  make all                           # build every checked-in environment"
	@echo "  make all JOBS=<n>                  # override the all-core default"
	@echo "  make clangd ENV=<environment>      # index one real cross-compiled target"
	@echo "  make format-cpp                    # format first-party C/C++ sources"
	@echo "  make format-check-cpp              # check first-party C/C++ formatting"
	@echo "  make probe ENV=<env> [PORT=<port>] # identify chip, security state, and flash size"
	@echo "  make backup ENV=<env> OUTPUT=<file> [PORT=<port>] # read a complete private backup"
	@echo "  make flash ENV=<env> [PORT=<port>] # select port, build, probe, and flash"
	@echo "  make flash-monitor ENV=<env> [PORT=<port>] # build, flash, and monitor"
	@echo "  make update ENV=<env> [PORT=<port>] # build and write only the app partition"
	@echo "  make flash-only PACKAGE=<manifest.json|factory.zip> [PORT=<port>] # flash without compiling"
	@echo "  make flash-only-monitor PACKAGE=<manifest.json|factory.zip> [PORT=<port>] # flash and monitor"
	@echo "  make monitor ENV=<env> [PORT=<port>] # open the platform serial monitor"
	@echo "  make clean                         # remove generated build output"
	@echo ""
	@echo "Environments: $(ENV_TARGETS)"
