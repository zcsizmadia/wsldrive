# Interface target carrying the compiler options for first-party code only.
# Third-party code pulled in via FetchContent does not link this target.
add_library(wsldrive_options INTERFACE)
add_library(wsldrive::options ALIAS wsldrive_options)

if(MSVC)
  target_compile_options(wsldrive_options INTERFACE
    /W4
    /permissive-
    /Zc:__cplusplus
    /Zc:preprocessor
    /utf-8
    /EHsc
    /w14265   # class has virtual functions but non-virtual destructor
    /w14062   # enumerator not handled in switch
    $<$<BOOL:${WSLDRIVE_WARNINGS_AS_ERRORS}>:/WX>
    $<$<CONFIG:Release,RelWithDebInfo>:/Oi /Gy /Gw>)
  target_compile_definitions(wsldrive_options INTERFACE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE _UNICODE)
  target_link_options(wsldrive_options INTERFACE
    $<$<CONFIG:Release,RelWithDebInfo>:/OPT:REF /OPT:ICF>
    $<$<BOOL:${WSLDRIVE_WARNINGS_AS_ERRORS}>:/WX>)
else()
  target_compile_options(wsldrive_options INTERFACE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wsign-conversion
    -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
    -Wnull-dereference -Wdouble-promotion -Wimplicit-fallthrough
    $<$<BOOL:${WSLDRIVE_WARNINGS_AS_ERRORS}>:-Werror>)
  target_link_options(wsldrive_options INTERFACE
    $<$<BOOL:${WSLDRIVE_WARNINGS_AS_ERRORS}>:-Wl,--fatal-warnings>)
endif()

# Link-time optimisation for optimised builds where the toolchain supports it.
include(CheckIPOSupported)
check_ipo_supported(RESULT WSLDRIVE_IPO_OK OUTPUT WSLDRIVE_IPO_MSG LANGUAGES CXX)
if(WSLDRIVE_IPO_OK)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
endif()
