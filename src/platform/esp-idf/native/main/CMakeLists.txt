idf_build_get_property(newmota_console NEWMOTA_CONSOLE)
idf_build_get_property(newmota_image_role NEWMOTA_IMAGE_ROLE)
idf_build_get_property(newmota_build_date NEWMOTA_BUILD_DATE)
idf_build_get_property(newmota_build_time NEWMOTA_BUILD_TIME)
idf_build_get_property(newmota_version NEWMOTA_VERSION)
idf_build_get_property(newmota_esp_layout_id NEWMOTA_ESP_LAYOUT_ID)
idf_build_get_property(newmota_esp_app0_offset NEWMOTA_ESP_APP0_OFFSET)
idf_build_get_property(newmota_esp_safeboot_offset NEWMOTA_ESP_SAFEBOOT_OFFSET)
idf_build_get_property(newmota_esp_otadata_offset NEWMOTA_ESP_OTADATA_OFFSET)
idf_build_get_property(newmota_esp_nvs_offset NEWMOTA_ESP_NVS_OFFSET)
idf_build_get_property(newmota_esp_psram_bytes NEWMOTA_ESP_PSRAM_BYTES)
idf_build_get_property(newmota_esp_sdk_target NEWMOTA_ESP_SDK_TARGET)
idf_build_get_property(newmota_feature_dir NEWMOTA_FEATURE_DIR)
idf_build_get_property(newmota_hardware_dir NEWMOTA_HARDWARE_DIR)
idf_build_get_property(newmota_berry_generated_dir NEWMOTA_BERRY_GENERATED_DIR)

include("${newmota_feature_dir}/requirements.cmake")

set(newmota_include_dirs
    "${NEWMOTA_ROOT}/src/app/include"
    "${NEWMOTA_ROOT}/src/platform/include"
    "${NEWMOTA_ROOT}/src/third_party/littlefs"
    "${newmota_feature_dir}"
    "${newmota_hardware_dir}"
)

if(NEWMOTA_USE_SDCARD)
  list(APPEND newmota_include_dirs "${NEWMOTA_ROOT}/src/third_party/fatfs")
endif()

if(NEWMOTA_USE_MQTT)
  list(APPEND newmota_include_dirs
    "${NEWMOTA_ROOT}/src/third_party/coremqtt/v2.3.1/include"
    "${NEWMOTA_ROOT}/src/third_party/coremqtt/v2.3.1/interface"
  )
endif()

if(NEWMOTA_USE_LVGL)
  list(APPEND newmota_include_dirs
    "${NEWMOTA_ROOT}/src/third_party/lvgl"
    "${NEWMOTA_ROOT}/src/third_party/lvgl/src"
    "${NEWMOTA_ROOT}/src/third_party/lv_binding_berry/src"
    "${NEWMOTA_ROOT}/src/third_party/lv_binding_berry/generate"
    "${NEWMOTA_ROOT}/src/third_party/berry_mapping/src"
    "${NEWMOTA_ROOT}/src/third_party/berry_ctypes"
  )
endif()

if(NEWMOTA_USE_BERRY AND NEWMOTA_USE_BLE AND NOT NEWMOTA_USE_LVGL)
  list(APPEND newmota_include_dirs "${NEWMOTA_ROOT}/src/third_party/berry_mapping/src")
endif()

if(NEWMOTA_USE_BERRY)
  list(APPEND newmota_include_dirs
    "${NEWMOTA_ROOT}/src/third_party/berry/src"
    "${newmota_berry_generated_dir}"
  )
endif()

idf_component_register(
  SRCS ${NEWMOTA_SOURCES}
  INCLUDE_DIRS ${newmota_include_dirs}
  PRIV_INCLUDE_DIRS
    "${NEWMOTA_ROOT}/src/app"
  PRIV_REQUIRES
    ${NEWMOTA_SDK_REQUIREMENTS}
)

# Header iteration is unavailable in the public HTTP server API. The adapter reads
# the pinned SDK's parsed header storage to honor duplicate Connection fields.
if(NEWMOTA_USE_WEB OR NEWMOTA_USE_RECOVERY_WEB)
  idf_component_get_property(newmota_http_server_dir esp_http_server COMPONENT_DIR)
  target_include_directories(${COMPONENT_LIB} PRIVATE
    "${newmota_http_server_dir}/src"
    "${newmota_http_server_dir}/src/port/esp32"
  )
endif()

# The vendored esp_h264 hardware encoder keeps its own include layout. Its CMake picks
# the register map by the minimum P4 silicon revision; so does this.
if(NEWMOTA_USE_CSI_WEBCAM)
  set(newmota_h264 "${NEWMOTA_ROOT}/src/third_party/esp_h264")
  if(CONFIG_ESP32P4_REV_MIN_FULL GREATER_EQUAL 300)
    set(newmota_h264_soc "${newmota_h264}/hw/soc/esp32p4/hw_ver3")
  else()
    set(newmota_h264_soc "${newmota_h264}/hw/soc/esp32p4/hw_ver1")
  endif()
  target_include_directories(${COMPONENT_LIB} PRIVATE
    "${newmota_h264}/interface/include"
    "${newmota_h264}/hw/include"
    "${newmota_h264}/port/include"
    "${newmota_h264}/port/inc"
    "${newmota_h264}/hw/src"
    "${newmota_h264}/hw/hal/esp32p4"
    "${newmota_h264}/hw/soc/esp32p4"
    "${newmota_h264_soc}"
  )
endif()

# JPEGDEC for the Berry img class; port/ wraps the unchanged sources, see its README.
# esp-dsp's FFT platform header tells it that the S3 vector unit exists.
if(NEWMOTA_USE_BERRY_IMAGE)
  target_include_directories(${COMPONENT_LIB} PRIVATE
    "${NEWMOTA_ROOT}/src/third_party/jpegdec/src"
    "${NEWMOTA_ROOT}/src/third_party/esp_dsp/modules/fft/include"
  )
endif()

# esp-dsp's FFT for the spectrum analyzer on the S3 and P4. Its Kconfig is not vendored, so the
# option that enables the P4 kernel and the optimized dispatch is set here.
if(NEWMOTA_USE_ESP_DSP)
  target_include_directories(${COMPONENT_LIB} PRIVATE
    "${NEWMOTA_ROOT}/src/third_party/esp_dsp/modules/common/include"
    "${NEWMOTA_ROOT}/src/third_party/esp_dsp/modules/fft/include"
  )
  target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_DSP_OPTIMIZED=1)
endif()

# ESP-IDF 6.x moved the PHY and SPI Ethernet drivers out of esp_eth; see their README.
if(NEWMOTA_USE_ETHERNET AND NOT EXISTS "$ENV{IDF_PATH}/components/esp_eth/src/spi")
  set(newmota_eth "${NEWMOTA_ROOT}/src/third_party/esp_eth_drivers")
  target_include_directories(${COMPONENT_LIB} PRIVATE
    "${newmota_eth}/ip101/include"
    "${newmota_eth}/lan87xx/include"
    "${newmota_eth}/rtl8201/include"
    "${newmota_eth}/dp83848/include"
    "${newmota_eth}/ksz80xx/include"
    "${newmota_eth}/dm9051/include"
    "${newmota_eth}/ksz8851snl/include"
    "${newmota_eth}/w5500/include"
    "${newmota_eth}/wiznet_common/include"
  )
endif()

# The ESP-IDF fork builds without mbedtls when no requirement selects it. bootloader_support
# then leaves its application-side image hash to the application, which uses BearSSL.
idf_build_get_property(newmota_build_components BUILD_COMPONENTS)
if(NOT mbedtls IN_LIST newmota_build_components)
  if(NOT NEWMOTA_USE_CRYPTO_SHA224_SHA256)
    # links_sdk_mbedtls() in tools/newmota/platforms/esp/build.py predicted mbedTLS.
    message(FATAL_ERROR "The build links no mbedTLS, but BearSSL SHA-256 for the SDK's image hash was not selected")
  endif()
  idf_component_get_property(newmota_bootloader_support_dir bootloader_support COMPONENT_DIR)
  set(newmota_sdk_image_sha256 "${NEWMOTA_ROOT}/src/platform/esp-idf/sdk_image_sha256.c")
  target_sources(${COMPONENT_LIB} PRIVATE "${newmota_sdk_image_sha256}")
  set_property(SOURCE "${newmota_sdk_image_sha256}" APPEND PROPERTY INCLUDE_DIRECTORIES
    "${newmota_bootloader_support_dir}/private_include")
  # bootloader_support is linked after main; keep the definitions in the link.
  target_link_options(${COMPONENT_LIB} INTERFACE "LINKER:-u,bootloader_sha256_start")
endif()

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
if(NEWMOTA_USE_CRYPTO)
  target_include_directories(${COMPONENT_LIB} PRIVATE ${NEWMOTA_CRYPTO_INCLUDE_DIRS})
  target_compile_definitions(${COMPONENT_LIB} PRIVATE ${NEWMOTA_CRYPTO_DEFINES})
endif()
if(NEWMOTA_USE_WIFI_WPA3_BEARSSL)
  # The hostap crypto.h backend builds against the supplicant's private headers.
  idf_component_get_property(newmota_wpa_dir wpa_supplicant COMPONENT_DIR)
  set_property(SOURCE "${NEWMOTA_ROOT}/src/platform/crypto/hostap_sae_crypto.c"
    "${NEWMOTA_ROOT}/src/platform/crypto/hostap_symmetric_bearssl.c" APPEND PROPERTY
    INCLUDE_DIRECTORIES "${newmota_wpa_dir}/src" "${newmota_wpa_dir}/src/utils")
  # wpa_supplicant is linked after main; keep the backend objects in the link.
  target_link_options(${COMPONENT_LIB} INTERFACE "LINKER:-u,crypto_bignum_init" "LINKER:-u,sha1_vector")
  if(NEWMOTA_USE_CRYPTO_HW_SHA1)
    # crypto_hash.c's __wrap_pbkdf2_sha1 (midstates, engine held) replaces hostap's
    # sha1-pbkdf2.c for every supplicant caller.
    target_link_options(${COMPONENT_LIB} INTERFACE "LINKER:--wrap=pbkdf2_sha1")
  endif()
endif()
if(mbedtls IN_LIST newmota_build_components)
  # The ESP32 hardware AES provider then shares the SDK driver's private engine lock.
  target_compile_definitions(${COMPONENT_LIB} PRIVATE NEWMOTA_SDK_MBEDTLS=1)
endif()
target_compile_options(${COMPONENT_LIB} PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
  $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
  LFS_MULTIVERSION
  LFS_NO_DEBUG
  LFS_NO_ERROR
  LFS_NO_MALLOC
  LFS_NO_WARN
  NEWMOTA_FILESYSTEM_ENABLED=1
  NEWMOTA_TARGET_NAME="${NEWMOTA_ENV}"
  NEWMOTA_FRAMEWORK_VERSION="${newmota_version}"
  NEWMOTA_BUILD_DATE="${newmota_build_date}"
  NEWMOTA_BUILD_TIME="${newmota_build_time}"
)

if(NEWMOTA_USE_MQTT)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE
    MQTT_DO_NOT_USE_CUSTOM_CONFIG=1
  )
endif()

if(newmota_image_role STREQUAL "safeboot")
  target_sources(${COMPONENT_LIB} PRIVATE
    "${NEWMOTA_ROOT}/src/platform/esp-idf/image_descriptor.cpp"
  )
  target_compile_definitions(${COMPONENT_LIB} PRIVATE
    NEWMOTA_IMAGE_SAFEBOOT=1
    NEWMOTA_ESP_LAYOUT_ID="${newmota_esp_layout_id}"
    NEWMOTA_ESP_APP0_OFFSET=${newmota_esp_app0_offset}
    NEWMOTA_ESP_SAFEBOOT_OFFSET=${newmota_esp_safeboot_offset}
    NEWMOTA_ESP_OTADATA_OFFSET=${newmota_esp_otadata_offset}
    NEWMOTA_ESP_NVS_OFFSET=${newmota_esp_nvs_offset}
    NEWMOTA_ESP_PSRAM_BYTES=${newmota_esp_psram_bytes}
    NEWMOTA_ESP_SDK_TARGET="${newmota_esp_sdk_target}"
    NEWMOTA_IMAGE_ENVIRONMENT="${NEWMOTA_ENV}"
    NEWMOTA_IMAGE_SDK_TARGET="${newmota_esp_sdk_target}"
    NEWMOTA_IMAGE_LAYOUT_ID="${newmota_esp_layout_id}"
    NEWMOTA_IMAGE_APP0_OFFSET=${newmota_esp_app0_offset}
    NEWMOTA_IMAGE_SAFEBOOT_OFFSET=${newmota_esp_safeboot_offset}
    NEWMOTA_IMAGE_OTADATA_OFFSET=${newmota_esp_otadata_offset}
    NEWMOTA_IMAGE_NVS_OFFSET=${newmota_esp_nvs_offset}
    NEWMOTA_IMAGE_PSRAM_BYTES=${newmota_esp_psram_bytes}
  )
  target_link_libraries(${COMPONENT_LIB} "-u custom_app_desc")
elseif(newmota_image_role STREQUAL "application")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE
    NEWMOTA=1
    NEWMOTA_IMAGE_APPLICATION=1
  )
  target_sources(${COMPONENT_LIB} PRIVATE
    "${NEWMOTA_ROOT}/src/platform/esp-idf/image_descriptor.cpp"
  )
  target_compile_definitions(${COMPONENT_LIB} PRIVATE
    NEWMOTA_IMAGE_ENVIRONMENT="${NEWMOTA_ENV}"
    NEWMOTA_IMAGE_SDK_TARGET="${newmota_esp_sdk_target}"
    NEWMOTA_IMAGE_LAYOUT_ID="${newmota_esp_layout_id}"
    NEWMOTA_IMAGE_APP0_OFFSET=${newmota_esp_app0_offset}
    NEWMOTA_IMAGE_SAFEBOOT_OFFSET=${newmota_esp_safeboot_offset}
    NEWMOTA_IMAGE_OTADATA_OFFSET=${newmota_esp_otadata_offset}
    NEWMOTA_IMAGE_NVS_OFFSET=${newmota_esp_nvs_offset}
    NEWMOTA_IMAGE_PSRAM_BYTES=${newmota_esp_psram_bytes}
  )
  target_link_libraries(${COMPONENT_LIB} "-u custom_app_desc")
else()
  message(FATAL_ERROR "Unsupported Newmota image role: ${newmota_image_role}")
endif()

if(newmota_console STREQUAL "usb-cdc")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE NEWMOTA_CONSOLE_USB_CDC=1)
elseif(newmota_console STREQUAL "usb-serial-jtag")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE NEWMOTA_CONSOLE_USB_SERIAL_JTAG=1)
elseif(NOT newmota_console STREQUAL "uart0")
  message(FATAL_ERROR "Unsupported Newmota ESP-IDF console: ${newmota_console}")
endif()
