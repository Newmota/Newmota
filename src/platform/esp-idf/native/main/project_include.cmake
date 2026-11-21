# Newmota's asymmetric app partitions hold different images. Keep ESP-IDF's
# overflow and near-full checks, but compare each image only with its destination.
if(NEWMOTA_IMAGE_ROLE STREQUAL "application")
  set(newmota_partition_subtype ota_0)
elseif(NEWMOTA_IMAGE_ROLE STREQUAL "safeboot")
  set(newmota_partition_subtype factory)
else()
  message(FATAL_ERROR "Unsupported Newmota image role: ${NEWMOTA_IMAGE_ROLE}")
endif()

# Dependencies load partition_table's helper before this component's hook.
# CMake retains the original function under its underscore-prefixed name.
function(partition_table_add_check_size_target target_name)
  if(target_name STREQUAL "app_check_size")
    _partition_table_add_check_size_target(${target_name} ${ARGN}
      PARTITION_SUBTYPE ${newmota_partition_subtype})
  else()
    _partition_table_add_check_size_target(${target_name} ${ARGN})
  endif()
endfunction()
