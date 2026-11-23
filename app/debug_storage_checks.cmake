# STruC++ v0.6.3 casts each leaf address to void* and tags it with
# an IEC wire type. The tag alone cannot distinguish incompatible C++
# storage (e.g. enum wrappers or non-default STRING capacities).
# Keep the original table as the sole source of addresses, IDs and tags.
function(retroplc_check_debug_storage debug_source output_file config_class)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${debug_source}")
  file(STRINGS "${debug_source}" lines ENCODING UTF-8)
  set(checks "")
  set(found_end FALSE)
  set(debug_config "")
  foreach(line IN LISTS lines)
    if(line MATCHES "^extern ::strucpp::([A-Za-z_][A-Za-z0-9_]*) g_config;$")
      set(debug_config "${CMAKE_MATCH_1}")
    elseif(line MATCHES "^const Entry debug_arr_0\\[([0-9]+)\\] STRUCPP_DEBUG_FLASH = \\{$")
      set(expected "${CMAKE_MATCH_1}")
      set(actual 0)
    elseif(DEFINED expected)
      if(line STREQUAL "};")
        set(found_end TRUE)
        break()
      elseif(line MATCHES "^    \\{ \\(void\\*\\)&(.+), (TAG_[A-Z0-9_]+), [0-9]+ \\},")
        string(APPEND checks
            "    debug_storage_matches<debug::${CMAKE_MATCH_2}, decltype(${CMAKE_MATCH_1})>(),\n")
        math(EXPR actual "${actual} + 1")
      elseif(expected EQUAL 1 AND line MATCHES "^    \\{ nullptr, 0, 0 \\},")
        set(expected 0)
      else()
        message(FATAL_ERROR "Unrecognized STruC++ debug array 0 entry in ${debug_source}")
      endif()
    endif()
  endforeach()
  if(NOT debug_config STREQUAL config_class)
    message(FATAL_ERROR
        "${debug_source} must declare ::g_config with RETROPLC_CONFIGURATION_CLASS=${config_class}; found '${debug_config}'")
  endif()
  if(NOT found_end OR NOT actual EQUAL expected)
    message(FATAL_ERROR "Could not validate STruC++ debug array 0 in ${debug_source}")
  endif()

  file(GENERATE OUTPUT "${output_file}"
      CONTENT "constexpr std::array<bool, ${actual}> debug_storage_valid = {\n${checks}};\n")
endfunction()
