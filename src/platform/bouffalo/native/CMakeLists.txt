cmake_minimum_required(VERSION 3.15)
project(newmota_bootstrap LANGUAGES NONE)

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Wi-Fi6 lwIP defaults omit hostname support. Apply it to the application and
# every SDK source together so struct netif has one consistent layout.
if(CHIP STREQUAL "bl616" OR CHIP STREQUAL "bl618dg")
  sdk_add_compile_definitions(LWIP_NETIF_HOSTNAME=1)
endif()

# The BL602 E24 core traps on LR/SC although the SDK compiles it for rv32imafc.
# Compile every SDK and Newmota source without the A extension, as Bouffalo's
# Matter build does; atomic_bl602.c provides the resulting library calls. Link
# options keep rv32imafc: the toolchain has no rv32imfc multilib, and the
# rv32imafc libraries contain no atomic instructions in the linked image.
if(CHIP STREQUAL "bl602")
  sdk_add_compile_options(-march=rv32imfc)
endif()

# BL602 starts the AP DHCP server with its default server address, which
# replaces any netif address. Match the 192.168.4.1 setup AP of other targets.
sdk_add_compile_definitions(DHCPD_SERVER_IP=\"192.168.4.1\")

# Wi-Fi6 SDK logging and optional features leave unused declarations. Keep
# these exclusions private to their SDK targets; declaration and qualifier
# warnings still need attention in the SDK fork.
if(CHIP STREQUAL "bl616" OR CHIP STREQUAL "bl618dg")
  if(TARGET libwpa_supplicant)
    target_compile_options(libwpa_supplicant PRIVATE
      -Wno-unused-variable
      -Wno-unused-but-set-variable
      -Wno-unused-function
    )
  endif()
  if(TARGET librfparam)
    target_compile_options(librfparam PRIVATE -Wno-unused-variable)
  endif()
endif()

# The pinned BouffaloSDK consumes these settings as preprocessor symbols.  The
# SDK's generated Kconfig header does not contain CONFIG_LOG_LEVEL when its
# own logging component is disabled, so make the resolved Newmota policy
# explicit for every SDK target as well as the application target.
if(DEFINED CONFIG_LOG_LEVEL)
  sdk_add_compile_definitions(-DCONFIG_LOG_LEVEL=${CONFIG_LOG_LEVEL})
endif()
if(CONFIG_LOG_DISABLE)
  sdk_add_compile_definitions(-DCONFIG_LOG_DISABLE=1)
endif()

# One legacy lwIP source undefines printf after the SDK's global suppression
# option has been applied and then calls it without including its declaration.
# Re-include the Newmota compatibility header after that source-level
# undefinition through a generated include overlay.  It is placed before the
# SDK include directories so the wrapper is selected for every log setting.
set(NEWMOTA_SDK_LOG_OVERLAY "${CMAKE_BINARY_DIR}/sdk_log_overlay")
file(MAKE_DIRECTORY "${NEWMOTA_SDK_LOG_OVERLAY}/lwip")
file(WRITE "${NEWMOTA_SDK_LOG_OVERLAY}/lwip/dhcp.h"
  "#ifndef NEWMOTA_BOUFFALO_SDK_DHCP_WRAPPER_INCLUDED\n"
  "#define NEWMOTA_BOUFFALO_SDK_DHCP_WRAPPER_INCLUDED\n"
  "#include_next <lwip/dhcp.h>\n"
  "#else\n"
  "#include_next <lwip/dhcp.h>\n"
  "#define NEWMOTA_BOUFFALO_SDK_LOG_REAPPLY\n"
  "#include \"${CMAKE_CURRENT_SOURCE_DIR}/sdk_log.h\"\n"
  "#undef NEWMOTA_BOUFFALO_SDK_LOG_REAPPLY\n"
  "#endif\n")
target_include_directories(sdk_intf_lib BEFORE INTERFACE "${NEWMOTA_SDK_LOG_OVERLAY}")
sdk_add_compile_options(
  "$<$<COMPILE_LANGUAGE:C>:-include${CMAKE_CURRENT_SOURCE_DIR}/sdk_log.h>"
)

if(NOT NEWMOTA_ROOT)
  message(FATAL_ERROR "NEWMOTA_ROOT is required")
endif()
if(NOT NEWMOTA_SDK_ROOT)
  message(FATAL_ERROR "NEWMOTA_SDK_ROOT is required")
endif()
foreach(required NEWMOTA_IMAGE_ROLE NEWMOTA_BUILD_DATE NEWMOTA_BUILD_TIME
                  NEWMOTA_FEATURE_DIR NEWMOTA_HARDWARE_DIR
                  NEWMOTA_BERRY_GENERATED_DIR NEWMOTA_FILESYSTEM_OFFSET
                  NEWMOTA_FILESYSTEM_SIZE NEWMOTA_APPLICATION_OFFSET
                  NEWMOTA_APPLICATION_SIZE NEWMOTA_SAFEBOOT_OFFSET
                  NEWMOTA_SAFEBOOT_SIZE NEWMOTA_UPDATE_CHIP_ID
                  NEWMOTA_UPDATE_FLASH_BYTES NEWMOTA_UPDATE_LAYOUT_SHA256)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

include("${NEWMOTA_FEATURE_DIR}/requirements.cmake")

# Keep Wi-Fi's DMA/RX buffers and lwIP pools in their SDK sections. BLE's native
# 32 KiB exchange reservation leaves insufficient WRAM for the default 64 KiB
# packet heap; use the SDK's supported heap-size override for this opt-in build.
if(CHIP STREQUAL "bl616" AND NEWMOTA_USE_BLE)
  sdk_add_compile_definitions(LWIP_HEAP_SIZE=32768)
endif()

# The BL602 SDK linker script omits C++ constructor arrays. Newmota has global
# service objects with references initialized by C++ constructors, so retain
# those entries in XIP and invoke them from entry.cpp.
if(CHIP STREQUAL "bl602")
  get_property(NEWMOTA_BOUFFALO_BASE_LINKER_SCRIPT GLOBAL PROPERTY LINKER_SCRIPT)
  if(NOT EXISTS "${NEWMOTA_BOUFFALO_BASE_LINKER_SCRIPT}")
    message(FATAL_ERROR "Bouffalo linker script was not selected by the SDK")
  endif()
  set(NEWMOTA_BOUFFALO_INIT_ARRAY_FRAGMENT
    "${NEWMOTA_ROOT}/src/platform/bouffalo/native/init_array_sections.ld")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${NEWMOTA_BOUFFALO_BASE_LINKER_SCRIPT}"
    "${NEWMOTA_BOUFFALO_INIT_ARRAY_FRAGMENT}"
  )
  file(READ "${NEWMOTA_BOUFFALO_BASE_LINKER_SCRIPT}" NEWMOTA_BOUFFALO_LINKER_SCRIPT)
  file(READ "${NEWMOTA_BOUFFALO_INIT_ARRAY_FRAGMENT}" NEWMOTA_BOUFFALO_INIT_ARRAY_SECTIONS)
  set(NEWMOTA_BOUFFALO_LINKER_MARKER "    .sysinit.array :")
  string(FIND "${NEWMOTA_BOUFFALO_LINKER_SCRIPT}" "${NEWMOTA_BOUFFALO_LINKER_MARKER}"
    NEWMOTA_BOUFFALO_LINKER_MARKER_INDEX
  )
  if(NEWMOTA_BOUFFALO_LINKER_MARKER_INDEX EQUAL -1)
    message(FATAL_ERROR "Bouffalo linker script is missing the .sysinit.array insertion point")
  endif()
  string(REPLACE "${NEWMOTA_BOUFFALO_LINKER_MARKER}"
    "${NEWMOTA_BOUFFALO_INIT_ARRAY_SECTIONS}\n${NEWMOTA_BOUFFALO_LINKER_MARKER}"
    NEWMOTA_BOUFFALO_LINKER_SCRIPT "${NEWMOTA_BOUFFALO_LINKER_SCRIPT}"
  )
  if(NEWMOTA_USE_BLE)
    # The SDK's fixed 1 MiB XIP region is unrelated to the selected Newmota slot.
    # Include the native 4 KiB image header in the catalog's complete image budget.
    math(EXPR NEWMOTA_BOUFFALO_XIP_SIZE "${NEWMOTA_APPLICATION_SIZE} - 4096")
    set(NEWMOTA_BOUFFALO_XIP_MARKER
      "xip_memory  (rx)  : ORIGIN = 0x23000000, LENGTH = 1024K")
    string(FIND "${NEWMOTA_BOUFFALO_LINKER_SCRIPT}" "${NEWMOTA_BOUFFALO_XIP_MARKER}" xip_marker)
    if(xip_marker EQUAL -1)
      message(FATAL_ERROR "BL602 SDK XIP layout changed; review BLE slot bounds")
    endif()
    string(REPLACE "${NEWMOTA_BOUFFALO_XIP_MARKER}"
      "xip_memory  (rx)  : ORIGIN = 0x23000000, LENGTH = ${NEWMOTA_BOUFFALO_XIP_SIZE}"
      NEWMOTA_BOUFFALO_LINKER_SCRIPT "${NEWMOTA_BOUFFALO_LINKER_SCRIPT}")
    # BLE owns the final 8 KiB of exchange RAM. lwIP memp pools contain CPU
    # protocol objects, not MAC DMA descriptors: keep those in normal zeroed RAM.
    # Preserve the SDK placement of the Wi-Fi descriptors and lwIP packet heap.
    set(NEWMOTA_BOUFFALO_WIFI_MARKER "      *(.wifi_ram*)")
    string(FIND "${NEWMOTA_BOUFFALO_LINKER_SCRIPT}" "${NEWMOTA_BOUFFALO_WIFI_MARKER}" wifi_marker)
    if(wifi_marker EQUAL -1)
      message(FATAL_ERROR "BL602 SDK Wi-Fi RAM layout changed; review BLE reservations")
    endif()
    string(REPLACE "${NEWMOTA_BOUFFALO_WIFI_MARKER}"
      "      *(EXCLUDE_FILE(*memp.c.obj) .wifi_ram*)"
      NEWMOTA_BOUFFALO_LINKER_SCRIPT "${NEWMOTA_BOUFFALO_LINKER_SCRIPT}")
    string(REPLACE "        *(.bss*)"
      "        *memp.c.obj(.wifi_ram*)\n        *(.bss*)"
      NEWMOTA_BOUFFALO_LINKER_SCRIPT "${NEWMOTA_BOUFFALO_LINKER_SCRIPT}")
  endif()
  set(NEWMOTA_BOUFFALO_LINKER_SCRIPT_OUTPUT
    "${CMAKE_CURRENT_BINARY_DIR}/newmota_bouffalo_linker.ld")
  file(WRITE "${NEWMOTA_BOUFFALO_LINKER_SCRIPT_OUTPUT}" "${NEWMOTA_BOUFFALO_LINKER_SCRIPT}")
  sdk_set_linker_script("${NEWMOTA_BOUFFALO_LINKER_SCRIPT_OUTPUT}")
endif()

sdk_add_include_directories(
  ${NEWMOTA_CRYPTO_INCLUDE_DIRS}
  "${NEWMOTA_ROOT}/src/app"
  "${NEWMOTA_ROOT}/src/app/include"
  "${NEWMOTA_ROOT}/src/platform/include"
  "${NEWMOTA_ROOT}/src/third_party/littlefs"
  "${NEWMOTA_SDK_ROOT}/components/net/lwip/lwip/lwip-port/config"
  "${NEWMOTA_ROOT}/config/build/bouffalo/${NEWMOTA_CHIP}"
  "${NEWMOTA_SDK_ROOT}/examples/boot2_isp/port/${CHIP}"
  "${NEWMOTA_FEATURE_DIR}"
  "${NEWMOTA_HARDWARE_DIR}"
)
if(NEWMOTA_USE_SDCARD)
  sdk_add_include_directories("${NEWMOTA_ROOT}/src/third_party/fatfs")
endif()
if(NEWMOTA_USE_MQTT)
  sdk_add_include_directories(
    "${NEWMOTA_ROOT}/src/third_party/coremqtt/v2.3.1/include"
    "${NEWMOTA_ROOT}/src/third_party/coremqtt/v2.3.1/interface"
  )
endif()
sdk_add_compile_options(
  $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
  $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)
sdk_add_compile_definitions(
  ${NEWMOTA_CRYPTO_DEFINES}
  NEWMOTA_TARGET_NAME="${NEWMOTA_ENV}"
  NEWMOTA_CHIP_NAME="${NEWMOTA_CHIP}"
  NEWMOTA_SDK_VERSION="${NEWMOTA_SDK_VERSION}"
  NEWMOTA_SDK_REVISION="${NEWMOTA_SDK_REVISION}"
  NEWMOTA_FRAMEWORK_VERSION="${NEWMOTA_VERSION}"
  NEWMOTA_BUILD_DATE="${NEWMOTA_BUILD_DATE}"
  NEWMOTA_BUILD_TIME="${NEWMOTA_BUILD_TIME}"
  # Enable the shared mount implementation and its LittleFS 2.0 disk-version field.
  NEWMOTA_FILESYSTEM_ENABLED=1
  LFS_MULTIVERSION
  NEWMOTA_FILESYSTEM_OFFSET=${NEWMOTA_FILESYSTEM_OFFSET}
  NEWMOTA_FILESYSTEM_SIZE=${NEWMOTA_FILESYSTEM_SIZE}
  NEWMOTA_FILESYSTEM_BLOCK_SIZE=4096
  NEWMOTA_APPLICATION_OFFSET=${NEWMOTA_APPLICATION_OFFSET}
  NEWMOTA_APPLICATION_SIZE=${NEWMOTA_APPLICATION_SIZE}
  NEWMOTA_SAFEBOOT_OFFSET=${NEWMOTA_SAFEBOOT_OFFSET}
  NEWMOTA_SAFEBOOT_SIZE=${NEWMOTA_SAFEBOOT_SIZE}
  NEWMOTA_UPDATE_CHIP_ID=${NEWMOTA_UPDATE_CHIP_ID}
  NEWMOTA_UPDATE_FLASH_BYTES=${NEWMOTA_UPDATE_FLASH_BYTES}
  NEWMOTA_UPDATE_LAYOUT_SHA256="${NEWMOTA_UPDATE_LAYOUT_SHA256}"
  LWIP_STATS_DISPLAY=1
)
if(NEWMOTA_USE_MQTT)
  sdk_add_compile_definitions(MQTT_DO_NOT_USE_CUSTOM_CONFIG=1)
endif()

# The SDK adds its main file separately.
list(REMOVE_ITEM NEWMOTA_SOURCES "${NEWMOTA_ROOT}/src/platform/bouffalo/native/entry.cpp")
target_sources(app PRIVATE ${NEWMOTA_SOURCES})
# Imported BearSSL and Berry intentionally test undefined feature, target, and
# compiler macros. Keep the SDK's -Wundef diagnostics for Newmota and all other sources.
foreach(source IN LISTS NEWMOTA_SOURCES)
  if(source MATCHES "^${NEWMOTA_ROOT}/src/third_party/(bearssl|berry)/src/")
    set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_OPTIONS -Wno-undef)
  endif()
endforeach()
if(NEWMOTA_USE_WIFI_WPA3_BEARSSL AND CHIP STREQUAL "bl602")
  # Only the hash vectors replace the Wi-Fi 4 supplicant's; its AES stays (wpa_aes_*).
  set(newmota_wifi4_supplicant "${NEWMOTA_SDK_ROOT}/components/wireless/wifi4/wpa_supplicant")
  set_property(SOURCE "${NEWMOTA_ROOT}/src/platform/crypto/hostap_symmetric_bearssl.c"
    APPEND PROPERTY COMPILE_DEFINITIONS NEWMOTA_HOSTAP_HASH_ONLY=1)
  set_property(SOURCE "${NEWMOTA_ROOT}/src/platform/crypto/hostap_symmetric_bearssl.c"
    APPEND PROPERTY INCLUDE_DIRECTORIES
      "${newmota_wifi4_supplicant}/port/include" "${newmota_wifi4_supplicant}/src")
  if(NEWMOTA_USE_CRYPTO_HW_SHA1)
    # crypto_hash.c's __wrap_pbkdf2_sha1 (midstates, engine held) replaces the SDK's
    # sha1-pbkdf2-bl.c for every supplicant caller.
    sdk_add_link_options(-Wl,--wrap=pbkdf2_sha1)
  endif()
elseif(NEWMOTA_USE_WIFI_WPA3_BEARSSL)
  # The Wi-Fi 6 sae.c takes crypto_ec_get_a() as const and does not free it.
  set_property(SOURCE "${NEWMOTA_ROOT}/src/platform/crypto/hostap_sae_crypto.c"
    APPEND PROPERTY COMPILE_DEFINITIONS NEWMOTA_HOSTAP_EC_A_BORROWED=1)
  if(NEWMOTA_USE_CRYPTO_HW_SHA1)
    # hostap_hash_bearssl.c's pbkdf2_sha1 hands off to crypto_hash.c's engine version.
    set_property(SOURCE "${NEWMOTA_ROOT}/src/platform/crypto/hostap_hash_bearssl.c"
      APPEND PROPERTY COMPILE_DEFINITIONS NEWMOTA_HOSTAP_HW_PBKDF2_SHA1=1)
  endif()
endif()
if(NEWMOTA_USE_BLE AND NEWMOTA_USE_BERRY)
  target_include_directories(app PRIVATE "${NEWMOTA_ROOT}/src/third_party/berry_mapping/src")
endif()
if(NEWMOTA_USE_LVGL)
  target_include_directories(app PRIVATE
    "${NEWMOTA_ROOT}/src/third_party/lvgl"
    "${NEWMOTA_ROOT}/src/third_party/lvgl/src"
    "${NEWMOTA_ROOT}/src/third_party/lv_binding_berry/src"
    "${NEWMOTA_ROOT}/src/third_party/lv_binding_berry/generate"
    "${NEWMOTA_ROOT}/src/third_party/berry_mapping/src"
    "${NEWMOTA_ROOT}/src/third_party/berry_ctypes"
  )
endif()
if(NEWMOTA_USE_BERRY)
  sdk_add_include_directories(
    "${NEWMOTA_ROOT}/src/third_party/berry/src"
    "${NEWMOTA_BERRY_GENERATED_DIR}"
  )
endif()
if(NEWMOTA_IMAGE_ROLE STREQUAL "application")
  sdk_add_compile_definitions(NEWMOTA=1 NEWMOTA_IMAGE_APPLICATION=1)
elseif(NEWMOTA_IMAGE_ROLE STREQUAL "safeboot")
  sdk_add_compile_definitions(NEWMOTA_IMAGE_SAFEBOOT=1)
else()
  message(FATAL_ERROR "Unsupported Newmota image role: ${NEWMOTA_IMAGE_ROLE}")
endif()

# The precompiled Wi-Fi6 host and MAC libraries print plain printf diagnostics,
# including Wi-Fi credentials. Link copies whose printf calls reach
# newmota_sdk_printf, which drops them; the SDK tree stays unchanged.
if(CHIP STREQUAL "bl616" OR CHIP STREQUAL "bl618dg")
  get_property(newmota_sdk_libs GLOBAL PROPERTY SDK_LIBS)
  set(newmota_linked_libs "")
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/quiet_sdk_libs")
  foreach(lib ${newmota_sdk_libs})
    get_filename_component(lib_name "${lib}" NAME)
    # Source-built SDK libraries such as libmacsw_os_adapter are target names.
    if(lib_name MATCHES "^lib(fhost|macsw)_.*\\.a$" AND EXISTS "${lib}")
      set(quiet_lib "${CMAKE_BINARY_DIR}/quiet_sdk_libs/${lib_name}")
      execute_process(
        COMMAND "${CROSS_COMPILE}objcopy" --redefine-sym printf=newmota_sdk_printf
                "${lib}" "${quiet_lib}"
        RESULT_VARIABLE quiet_result)
      if(NOT quiet_result EQUAL 0)
        message(FATAL_ERROR "Cannot redirect printf in ${lib}")
      endif()
      list(APPEND newmota_linked_libs "${quiet_lib}")
    else()
      list(APPEND newmota_linked_libs "${lib}")
    endif()
  endforeach()
  set_property(GLOBAL PROPERTY SDK_LIBS ${newmota_linked_libs})
endif()

sdk_set_main_file("${NEWMOTA_ROOT}/src/platform/bouffalo/native/entry.cpp")
project(newmota)

# BLE records and partition ownership require the selected native host policy.
if(NEWMOTA_USE_BLE)
  if(NOT NEWMOTA_IMAGE_ROLE STREQUAL "application" OR CONFIG_NIMBLE OR CONFIG_BT_SETTINGS
     OR CONFIG_DISABLE_BT_SMP OR NOT CONFIG_BT_GEN_RANDOM_BY_SW)
    message(FATAL_ERROR "Bouffalo BLE requires application blestack, SMP, HCI-seeded PRNG and no EasyFlash settings")
  endif()
  if(CHIP STREQUAL "bl602")
    if(NOT CONFIG_BLECONTROLLER_LIB STREQUAL "m1s1")
      message(FATAL_ERROR "BL602 BLE requires the native m1s1 controller configuration")
    endif()
  elseif(CHIP STREQUAL "bl616")
    if(NOT CONFIG_BTBLECONTROLLER_LIB STREQUAL "ble1m10s1bredr0")
      message(FATAL_ERROR "BL616 BLE requires the native ble1m10s1bredr0 controller")
    endif()
  else()
    message(FATAL_ERROR "Bouffalo BLE is implemented for BL602 and BL616; BL618DG is deferred")
  endif()
endif()
