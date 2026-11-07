cmake_minimum_required(VERSION 3.24)
project(m_ui_preset_aviutl2 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "AviUtl2 plugins can only be built on Windows.")
endif()

include("${CMAKE_SOURCE_DIR}/cmake/AviUtl2Plugin.cmake")

aviutl2_add_plugin(m_ui_preset_aviutl2
  TYPE aux2
  SOURCES plugins/m_ui_preset_aviutl2/m_ui_preset_aviutl2.cpp
)

# AviUtl本体は C++ 例外 (0xE06D7363) を投げることがあるため、SEH も捕捉する
target_compile_options(m_ui_preset_aviutl2 PRIVATE /EHa)
target_link_libraries(m_ui_preset_aviutl2 PRIVATE comctl32 shell32 user32)
