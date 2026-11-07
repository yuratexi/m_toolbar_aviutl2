# Helpers for AviUtl2 plugin DLL targets.

set(AVIUTL2_SDK_DIR "${CMAKE_SOURCE_DIR}/sdk/include" CACHE PATH "AviUtl2 Plugin SDK include directory")

function(aviutl2_add_plugin target)
  set(options)
  set(oneValueArgs TYPE OUTPUT_NAME)
  set(multiValueArgs SOURCES LIBRARIES)
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT ARG_TYPE)
    message(FATAL_ERROR "aviutl2_add_plugin(${target}): TYPE is required (aui2|auo2|auf2|aux2|mod2)")
  endif()
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "aviutl2_add_plugin(${target}): SOURCES is required")
  endif()
  if(NOT ARG_OUTPUT_NAME)
    set(ARG_OUTPUT_NAME "${target}")
  endif()

  add_library(${target} SHARED ${ARG_SOURCES})
  target_include_directories(${target} PRIVATE "${AVIUTL2_SDK_DIR}")
  # C++17 keeps u8"..." as const char* (SDK samples use u8 strings as LPCSTR).
  target_compile_features(${target} PRIVATE cxx_std_17)
  target_compile_definitions(${target} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(${target} PRIVATE /utf-8 /W3)

  if(ARG_LIBRARIES)
    target_link_libraries(${target} PRIVATE ${ARG_LIBRARIES})
  endif()

  set_target_properties(${target} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${ARG_OUTPUT_NAME}"
    SUFFIX ".${ARG_TYPE}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/plugins"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/plugins"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/plugins"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/plugins"
  )
endfunction()
