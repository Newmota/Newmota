cmake_minimum_required(VERSION 3.13)

foreach(required NEWMOTA_ROOT NEWMOTA_ENV NEWMOTA_CHIP NEWMOTA_SDK_VERSION
                 NEWMOTA_VERSION NEWMOTA_BUILD_DATE NEWMOTA_BUILD_TIME
                 NEWMOTA_IMAGE_ROLE
                 NEWMOTA_FEATURE_DIR
                 NEWMOTA_HARDWARE_DIR NEWMOTA_BERRY_GENERATED_DIR
                 NEWMOTA_PARTITION_JSON NEWMOTA_SLOT_BASE NEWMOTA_SLOT_SIZE
                 NEWMOTA_FILESYSTEM_OFFSET NEWMOTA_FILESYSTEM_SIZE
                 NEWMOTA_FILESYSTEM_BLOCK_SIZE NEWMOTA_APPLICATION_OFFSET
                 NEWMOTA_APPLICATION_SIZE NEWMOTA_SAFEBOOT_OFFSET
                 NEWMOTA_SAFEBOOT_SIZE NEWMOTA_BOOT_MARKER_OFFSET
                 NEWMOTA_UPDATE_CHIP_ID NEWMOTA_UPDATE_FLASH_BYTES
                 NEWMOTA_UPDATE_LAYOUT_SHA256 NEWMOTA_DATA_OFFSET
                 NEWMOTA_DATA_SIZE)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()
if(NOT DEFINED NEWMOTA_PICO_RP2040)
  message(FATAL_ERROR "NEWMOTA_PICO_RP2040 is required")
endif()

# These Pico settings are compile definitions consumed by SDK libraries, not
# ordinary CMake cache values.  Install them in the directory scope before
# pico_sdk_init() creates those libraries so the selected policy reaches both
# the SDK and the Newmota target.
foreach(sdk_log_setting PICO_CYW43_ARCH_DEBUG_ENABLED
                        PICO_CYW43_LOGGING_ENABLED
                        PICO_DEBUG_MALLOC)
  if(DEFINED ${sdk_log_setting})
    add_compile_definitions(${sdk_log_setting}=${${sdk_log_setting}})
  endif()
endforeach()

# The SDK default reserves only 2 KiB for the core-0 application stack.
# Newmota's main() and runtime need an 8 KiB floor. Core 1 is not used, so
# combine both 4 KiB scratch banks into the core-0 stack region.
if(NEWMOTA_USE_AUDIO_OPUS)
  # The carried fixed-point codec uses VLAs. Reserve Tasmota's capture-stack
  # budget at 48 kHz stereo (30000 + 48000/2*2), rounded to 80 KiB.
  # This is a reservation policy; hardware watermarks must qualify its margin.
  set(NEWMOTA_PICO_STACK_SIZE 0x14000)
elseif(NEWMOTA_USE_WEB_HTTPS)
  set(NEWMOTA_PICO_STACK_SIZE 0x8000)
else()
  set(NEWMOTA_PICO_STACK_SIZE 0x2000)
endif()
if(NEWMOTA_USE_WEB_HTTPS)
  # TLS callbacks are serviced by the cooperative application loop and need
  # room for the Mbed TLS handshake and certificate setup on the main stack.
  add_compile_definitions(LWIP_ALTCP=1 LWIP_ALTCP_TLS=1 LWIP_ALTCP_TLS_MBEDTLS=1)
endif()
add_compile_definitions(PICO_STACK_SIZE=${NEWMOTA_PICO_STACK_SIZE} PICO_CORE1_STACK_SIZE=0)
if(NEWMOTA_PICO_RP2040)
  # RP2040 Newmota never launches core 1. Without this SDK assumption,
  # flash_safe_execute() rejects writes because core 1 is not participating in
  # multicore lockout. RP2350 keeps the SDK lockout policy used by its tested
  # ROM flash path.
  add_compile_definitions(PICO_FLASH_ASSUME_CORE1_SAFE=1)

  # Temporary isolation policy: mask interrupts around SDK division and
  # place it in RAM. This is not evidence of a divider defect; SDK division
  # normally preserves interrupted divider state. Revisit after stable boot.
  add_compile_definitions(PICO_DIVIDER_DISABLE_INTERRUPTS=1
                          PICO_DIVIDER_IN_RAM=1)
endif()

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(newmota LANGUAGES C CXX ASM)
pico_sdk_init()

include("${NEWMOTA_FEATURE_DIR}/requirements.cmake")
if(NEWMOTA_USE_BLE)
  # The SDK lists both database implementations, but its TLV source requires
  # flash-pool macros unconditionally. Select the native RAM database only.
  get_target_property(newmota_btstack_ble_sources pico_btstack_ble INTERFACE_SOURCES)
  list(FILTER newmota_btstack_ble_sources EXCLUDE REGEX "/le_device_db_tlv\\.c$")
  set_property(TARGET pico_btstack_ble PROPERTY INTERFACE_SOURCES "${newmota_btstack_ble_sources}")
endif()
add_executable(newmota ${NEWMOTA_SOURCES})

# Boot-loop tracing remains available through NEWMOTA_PICO_BOOT_TRACE for
# diagnostic builds. Normal firmware uses the standard USB connection wait.
set(pico_stdio_usb_connect_wait_timeout_ms 250)

if(NEWMOTA_USE_WEB_HTTPS OR NEWMOTA_USE_AUDIO_OPUS)
  # Cooperative TLS and Opus share core 0. The scratch banks cannot hold their
  # stack; reserve its selected size at the top of regular SRAM instead.
  if(NEWMOTA_PICO_RP2040)
    math(EXPR NEWMOTA_PICO_RAM_LENGTH "0x40000 - ${NEWMOTA_PICO_STACK_SIZE}" OUTPUT_FORMAT HEXADECIMAL)
    math(EXPR NEWMOTA_PICO_STACK_ORIGIN "0x20000000 + ${NEWMOTA_PICO_RAM_LENGTH}" OUTPUT_FORMAT HEXADECIMAL)
    pico_set_linker_script_var(newmota RAM_LENGTH ${NEWMOTA_PICO_RAM_LENGTH})
    pico_set_linker_script_var(newmota SCRATCH_X_LENGTH 0)
    pico_set_linker_script_var(newmota SCRATCH_Y_ORIGIN ${NEWMOTA_PICO_STACK_ORIGIN})
  else()
    math(EXPR NEWMOTA_PICO_RAM_LENGTH "0x80000 - ${NEWMOTA_PICO_STACK_SIZE}" OUTPUT_FORMAT HEXADECIMAL)
    math(EXPR NEWMOTA_PICO_STACK_ORIGIN "0x20000000 + ${NEWMOTA_PICO_RAM_LENGTH}" OUTPUT_FORMAT HEXADECIMAL)
    pico_set_linker_script_var(newmota RAM_LENGTH ${NEWMOTA_PICO_RAM_LENGTH})
    pico_set_linker_script_var(newmota SCRATCH_Y_ORIGIN ${NEWMOTA_PICO_STACK_ORIGIN})
  endif()
  pico_set_linker_script_var(newmota SCRATCH_Y_LENGTH ${NEWMOTA_PICO_STACK_SIZE})
elseif(NEWMOTA_PICO_RP2040)
  # Newmota uses no scratch-memory code and core 1 is unused, so both 4 KiB
  # scratch banks back the normal 8 KiB core-0 stack. SCRATCH_X stays empty.
  pico_set_linker_script_var(newmota SCRATCH_X_LENGTH 0)
  pico_set_linker_script_var(newmota SCRATCH_Y_ORIGIN 0x20040000)
  pico_set_linker_script_var(newmota SCRATCH_Y_LENGTH 0x2000)
else()
  # RP2350's two 4 KiB scratch banks start at 0x20080000.
  pico_set_linker_script_var(newmota SCRATCH_Y_ORIGIN 0x20080000)
  pico_set_linker_script_var(newmota SCRATCH_Y_LENGTH 0x2000)
endif()

# RP2350 images link at the ROM's XIP runtime base because rom_chain_image()
# rolls the selected physical partition into that window. RP2040 has no such
# remapping and links each role at its physical XIP address. PICO_FLASH_SIZE_BYTES
# continues to describe the selected board's full physical flash capacity.
configure_file(
  "${CMAKE_CURRENT_LIST_DIR}/pico_flash_region.ld.in"
  "${CMAKE_CURRENT_BINARY_DIR}/newmota_flash/pico_flash_region.ld"
  @ONLY
)
pico_add_linker_script_override_path(newmota
  "${CMAKE_CURRENT_BINARY_DIR}/newmota_flash" FILES pico_flash_region.ld)

# RP2350 Safeboot is the ROM's first image and owns the resident partition
# table. RP2040 has no partition-table ROM API and uses a project-owned
# boot2/vector chain instead.
if(NOT NEWMOTA_PICO_RP2040 AND NEWMOTA_IMAGE_ROLE STREQUAL "safeboot")
  pico_embed_pt_in_binary(newmota "${NEWMOTA_PARTITION_JSON}")
endif()

target_include_directories(newmota PRIVATE
  "${NEWMOTA_ROOT}/src/app"
  "${NEWMOTA_ROOT}/src/app/include"
  "${NEWMOTA_ROOT}/src/platform/include"
  "${NEWMOTA_ROOT}/src/platform/pico-sdk"
  "${NEWMOTA_ROOT}/src/third_party/littlefs"
  "${NEWMOTA_FEATURE_DIR}"
  "${NEWMOTA_HARDWARE_DIR}"
)
if(NEWMOTA_USE_SDCARD)
  target_include_directories(newmota PRIVATE "${NEWMOTA_ROOT}/src/third_party/fatfs")
endif()
if(NEWMOTA_USE_MQTT)
  target_include_directories(newmota PRIVATE
    "${NEWMOTA_ROOT}/src/third_party/coremqtt/v2.3.1/include"
    "${NEWMOTA_ROOT}/src/third_party/coremqtt/v2.3.1/interface"
  )
endif()
if(NEWMOTA_USE_LVGL)
  target_include_directories(newmota PRIVATE
    "${NEWMOTA_ROOT}/src/third_party/lvgl"
    "${NEWMOTA_ROOT}/src/third_party/lvgl/src"
    "${NEWMOTA_ROOT}/src/third_party/lv_binding_berry/src"
    "${NEWMOTA_ROOT}/src/third_party/lv_binding_berry/generate"
    "${NEWMOTA_ROOT}/src/third_party/berry_mapping/src"
    "${NEWMOTA_ROOT}/src/third_party/berry_ctypes"
  )
endif()
if(NEWMOTA_USE_BLE)
  target_include_directories(newmota PRIVATE
    "${NEWMOTA_ROOT}/src/third_party/berry_mapping/src")
endif()
if(NEWMOTA_USE_BERRY)
  target_include_directories(newmota PRIVATE
    "${NEWMOTA_ROOT}/src/third_party/berry/src"
    "${NEWMOTA_BERRY_GENERATED_DIR}"
  )
endif()
if(NEWMOTA_USE_CMSIS_DSP)
  target_include_directories(newmota PRIVATE
    "${NEWMOTA_ROOT}/src/third_party/cmsis_dsp/Include"
    "${NEWMOTA_ROOT}/src/third_party/cmsis_dsp/PrivateInclude"
  )
endif()
target_compile_features(newmota PRIVATE cxx_std_17)
if(NEWMOTA_USE_CRYPTO)
  target_include_directories(newmota PRIVATE ${NEWMOTA_CRYPTO_INCLUDE_DIRS})
  target_compile_definitions(newmota PRIVATE ${NEWMOTA_CRYPTO_DEFINES})
endif()
target_compile_options(newmota PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
  $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)
target_compile_definitions(newmota PRIVATE
  # The Pico native adapter supplies the resolved LittleFS block device below;
  # keep the portable filesystem core enabled for both application and
  # Safeboot images.  Without this definition filesystem.cpp intentionally
  # compiles its unavailable stub, so a correctly flashed LittleFS volume
  # would still be reported as unmounted at runtime.
  NEWMOTA_FILESYSTEM_ENABLED=1
  # Match the filesystem image's LittleFS 2.0 on-disk version and expose the
  # disk_version field used by the portable mount configuration.
  LFS_MULTIVERSION
  NEWMOTA_TARGET_NAME="${NEWMOTA_ENV}"
  NEWMOTA_CHIP_NAME="${NEWMOTA_CHIP}"
  NEWMOTA_SDK_VERSION="${NEWMOTA_SDK_VERSION}"
  NEWMOTA_FRAMEWORK_VERSION="${NEWMOTA_VERSION}"
  NEWMOTA_BUILD_DATE="${NEWMOTA_BUILD_DATE}"
  NEWMOTA_BUILD_TIME="${NEWMOTA_BUILD_TIME}"
  NEWMOTA_SLOT_BASE=${NEWMOTA_SLOT_BASE}
  NEWMOTA_SLOT_SIZE=${NEWMOTA_SLOT_SIZE}
  NEWMOTA_FILESYSTEM_OFFSET=${NEWMOTA_FILESYSTEM_OFFSET}
  NEWMOTA_FILESYSTEM_SIZE=${NEWMOTA_FILESYSTEM_SIZE}
  NEWMOTA_FILESYSTEM_BLOCK_SIZE=${NEWMOTA_FILESYSTEM_BLOCK_SIZE}
  NEWMOTA_APPLICATION_OFFSET=${NEWMOTA_APPLICATION_OFFSET}
  NEWMOTA_APPLICATION_SIZE=${NEWMOTA_APPLICATION_SIZE}
  NEWMOTA_SAFEBOOT_OFFSET=${NEWMOTA_SAFEBOOT_OFFSET}
  NEWMOTA_SAFEBOOT_SIZE=${NEWMOTA_SAFEBOOT_SIZE}
  NEWMOTA_BOOT_MARKER_OFFSET=${NEWMOTA_BOOT_MARKER_OFFSET}
  NEWMOTA_DATA_OFFSET=${NEWMOTA_DATA_OFFSET}
  NEWMOTA_DATA_SIZE=${NEWMOTA_DATA_SIZE}
  NEWMOTA_UPDATE_CHIP_ID=${NEWMOTA_UPDATE_CHIP_ID}
  NEWMOTA_UPDATE_FLASH_BYTES=${NEWMOTA_UPDATE_FLASH_BYTES}
  NEWMOTA_UPDATE_LAYOUT_SHA256="${NEWMOTA_UPDATE_LAYOUT_SHA256}"
  PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=${pico_stdio_usb_connect_wait_timeout_ms}
)
if(NEWMOTA_USE_MQTT)
  target_compile_definitions(newmota PRIVATE MQTT_DO_NOT_USE_CUSTOM_CONFIG=1)
endif()
if(NEWMOTA_IMAGE_ROLE STREQUAL "application")
  target_compile_definitions(newmota PRIVATE NEWMOTA=1 NEWMOTA_IMAGE_APPLICATION=1)
elseif(NEWMOTA_IMAGE_ROLE STREQUAL "safeboot")
  target_compile_definitions(newmota PRIVATE NEWMOTA_IMAGE_SAFEBOOT=1)
else()
  message(FATAL_ERROR "Unsupported Newmota image role: ${NEWMOTA_IMAGE_ROLE}")
endif()
target_link_libraries(newmota PRIVATE ${NEWMOTA_SDK_REQUIREMENTS})
if(NEWMOTA_USE_BLE)
  # Clear reused RAM key slots before SM fills this pairing's distributed fields.
  target_link_options(newmota PRIVATE "LINKER:--wrap=le_device_db_add")
  target_compile_definitions(newmota PRIVATE
    NEWMOTA_PICO_BLE_SDK_REVISION="${NEWMOTA_PICO_BLE_SDK_REVISION}")
endif()

# NOTE: PICO_CRT0_IMAGE_TYPE_TBYB stays at the SDK default (0) on both
# roles so factory provisioning and normal boots just work; a TBYB-flagged
# image requires a flash-update boot and would be skipped by normal boot.
# Trial semantics can be re-enabled per role once flash-update
# provisioning is qualified on hardware.
pico_enable_stdio_usb(newmota 1)
pico_enable_stdio_uart(newmota 0)
pico_add_extra_outputs(newmota)
# The application UF2 targets its physical flash partition. RP2350's ROM
# chain maps that partition to XIP_BASE; RP2040 links and executes it at the
# physical XIP address after the resident Safeboot image.
if(NEWMOTA_IMAGE_ROLE STREQUAL "application")
  math(EXPR application_uf2_package_address
    "0x10000000 + ${NEWMOTA_APPLICATION_OFFSET}" OUTPUT_FORMAT HEXADECIMAL)
  get_property(picotool_path TARGET picotool PROPERTY LOCATION)
  if(NEWMOTA_PICO_RP2040)
    add_custom_command(TARGET newmota POST_BUILD
      COMMAND "${picotool_path}" uf2 convert --quiet
        "$<TARGET_FILE_DIR:newmota>/newmota.bin"
        "$<TARGET_FILE_DIR:newmota>/newmota.uf2"
        --offset "${application_uf2_package_address}"
        --family rp2040 --platform rp2040
      COMMENT "Packaging the RP2040 application UF2 at its flash partition"
      VERBATIM)
  else()
    add_custom_command(TARGET newmota POST_BUILD
      COMMAND "${picotool_path}" uf2 convert --quiet
        "$<TARGET_FILE_DIR:newmota>/newmota.bin"
        "$<TARGET_FILE_DIR:newmota>/newmota.uf2"
        --offset "${application_uf2_package_address}"
        --family rp2350-arm-s --platform rp2350 --abs-block
      COMMENT "Packaging the RP2350 application UF2 at its flash partition"
      VERBATIM)
  endif()
endif()
