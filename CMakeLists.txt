cmake_minimum_required(VERSION 3.15)

# Set CMAKE_PREFIX_PATH early to find Kodi-provided dependencies
if(NOT CMAKE_PREFIX_PATH)
  if(DEFINED ENV{KODI_BUILD_DIR})
    list(APPEND CMAKE_PREFIX_PATH "${ENV{KODI_BUILD_DIR}}/project/BuildDependencies/x64")
  else()
    # Fallback for standard Kodi location
    list(APPEND CMAKE_PREFIX_PATH "C:/Users/joe/dev/pvr/xbmc/project/BuildDependencies/x64")
  endif()
endif()

set(ADDON_ID "pvr.dispatcharr")
set(ADDON_NAME "Dispatcharr PVR Client")

project(${ADDON_ID} LANGUAGES CXX)

include(CTest)
if(BUILD_TESTING)
  add_executable(hls_playlist_test
    tests/hls_playlist_test.cpp
    src/recording/hls_playlist.cpp
  )
  target_include_directories(hls_playlist_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
  add_test(NAME hls_playlist_test COMMAND hls_playlist_test)
  add_executable(epg_recording_match_test
    tests/epg_recording_match_test.cpp
    src/recording/epg_recording_match.cpp
  )
  target_include_directories(epg_recording_match_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
  add_test(NAME epg_recording_match_test COMMAND epg_recording_match_test)
endif()

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR})

# Try to use Kodi's official addon build system when available.
find_package(Kodi QUIET)

if(Kodi_FOUND)

  
  # Read version from version.txt (after project() declaration)
  file(READ ${CMAKE_CURRENT_SOURCE_DIR}/version.txt ADDON_VERSION)
  string(STRIP "${ADDON_VERSION}" ADDON_VERSION)
  
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)

  # For Kodi addon builds, try to use CURL from dependencies first
  # Check if CURL was already provided by Kodi's dependency system
  set(CURL_INCLUDE_DIR)
  set(CURL_LIBRARY)
  
  # Try finding CURL in the CMAKE_PREFIX_PATH (set by Kodi)
  find_package(CURL QUIET)
  
  if(NOT CURL_FOUND)
    # If not found via standard find, try to locate it in xbmc BuildDependencies as fallback
    find_path(CURL_INCLUDE_DIR NAMES curl/curl.h
              PATHS "C:/Users/joe/dev/pvr/xbmc/project/BuildDependencies/x64/include"
              NO_DEFAULT_PATH)
    find_library(CURL_LIBRARY NAMES libcurl
                 PATHS "C:/Users/joe/dev/pvr/xbmc/project/BuildDependencies/x64/lib"
                 NO_DEFAULT_PATH)
    if(CURL_INCLUDE_DIR AND CURL_LIBRARY)
      message(STATUS "Found CURL in xbmc BuildDependencies")
      set(CURL_FOUND TRUE)
    endif()
  endif()
  
  if(NOT CURL_FOUND)
    message(FATAL_ERROR "CURL library not found. Please ensure Kodi dependencies are properly downloaded.")
  endif()
  
  # Try to find system pugixml, otherwise use bundled version
  find_package(pugixml QUIET)
  
  if(pugixml_FOUND)
    message(STATUS "Using system pugixml")
    include_directories(${KODI_INCLUDE_DIR}/..
                        ${PUGIXML_INCLUDE_DIRS}
                        ${CURL_INCLUDE_DIR})
    set(DEPLIBS ${PUGIXML_LIBRARIES} ${CURL_LIBRARY})
  else()
    message(STATUS "Using bundled pugixml")
    include_directories(${KODI_INCLUDE_DIR}/..
                        ${CMAKE_SOURCE_DIR}/src/pugixml
                        ${CURL_INCLUDE_DIR})
    set(DEPLIBS ${CURL_LIBRARY})
  endif()

  set(XTC_SOURCES
    src/addon.cpp
    src/xtream_client.cpp
    src/dispatcharr_client.cpp
    src/recording/hls_playlist.cpp
    src/recording/growing_recorded_stream.cpp
    src/recording/epg_recording_match.cpp
    src/recording/remote_file_recorded_stream.cpp
  )

  set(XTC_HEADERS
    src/dispatcharr_client.h
    src/xtream_client.h
    src/json.hpp
  )
  
  # Add bundled pugixml if not using system version
  if(NOT pugixml_FOUND)
    list(APPEND XTC_SOURCES src/pugixml/pugixml.cpp)
  endif()

  set(XTC_RESOURCES
    ${ADDON_ID}/addon.xml.in
    ${ADDON_ID}/resources/settings.xml
    ${ADDON_ID}/resources/language/resource.language.en_gb/strings.po
  )

  message(STATUS "DEPLIBS: ${DEPLIBS}")
  
  build_addon(${ADDON_ID} XTC DEPLIBS)

  target_compile_definitions(${ADDON_ID} PRIVATE 
    ADDON_ID="${ADDON_ID}"
    ADDON_NAME="${ADDON_NAME}" 
    ADDON_VERSION="${ADDON_VERSION}"
  )

  include(CPack)
else()
  # Standalone build using only the dev kit headers.

  # Read version from version.txt
  file(READ ${CMAKE_CURRENT_SOURCE_DIR}/version.txt ADDON_VERSION)
  string(STRIP "${ADDON_VERSION}" ADDON_VERSION)

  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)

  # Find libcurl
  find_package(CURL REQUIRED)
  message(STATUS "Found CURL: ${CURL_LIBRARIES}")
  message(STATUS "CURL include dirs: ${CURL_INCLUDE_DIRS}")

  # KODI_ADDON_SDK only needed for the main addon
  if(NOT DEFINED KODI_ADDON_SDK)
    message(FATAL_ERROR "KODI_ADDON_SDK not set. Configure with -DKODI_ADDON_SDK=/path/to/kodi-addon-dev-kit")
  endif()

  if(NOT EXISTS "${KODI_ADDON_SDK}/include/kodi/addon-instance/PVR.h")
    message(FATAL_ERROR "KODI_ADDON_SDK is invalid: missing include/kodi/addon-instance/PVR.h")
  endif()

  if(WIN32)
    add_library(${ADDON_ID} SHARED
      src/addon.cpp
      src/xtream_client.cpp
      src/dispatcharr_client.cpp
      src/recording/hls_playlist.cpp
      src/recording/growing_recorded_stream.cpp
      src/recording/epg_recording_match.cpp
      src/recording/remote_file_recorded_stream.cpp
      src/pugixml/pugixml.cpp
    )
  else()
    add_library(${ADDON_ID} MODULE
      src/addon.cpp
      src/xtream_client.cpp
      src/dispatcharr_client.cpp
      src/recording/hls_playlist.cpp
      src/recording/growing_recorded_stream.cpp
      src/recording/epg_recording_match.cpp
      src/recording/remote_file_recorded_stream.cpp
      src/pugixml/pugixml.cpp
    )
  endif()

  target_include_directories(${ADDON_ID} PRIVATE
    "${KODI_ADDON_SDK}/include"
    "${CMAKE_SOURCE_DIR}/src/pugixml"
    ${CURL_INCLUDE_DIRS}
  )

  # Link libcurl
  target_link_libraries(${ADDON_ID} PRIVATE ${CURL_LIBRARIES})

  set_target_properties(${ADDON_ID} PROPERTIES PREFIX "")

  if(WIN32)
    # Exception handling and RTTI  
    target_compile_options(${ADDON_ID} PRIVATE /EHsc /GR)
    # Define common Windows macros
    target_compile_definitions(${ADDON_ID} PRIVATE 
      _CRT_SECURE_NO_WARNINGS
      WIN32_LEAN_AND_MEAN
      NOMINMAX
    )
  endif()

  # === DEBUG: Print build configuration ===
  message(STATUS "=== ADDON BUILD CONFIGURATION ===")
  message(STATUS "Target: ${ADDON_ID}")
  message(STATUS "CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE}")
  message(STATUS "CMAKE_CXX_COMPILER: ${CMAKE_CXX_COMPILER}")
  message(STATUS "CMAKE_CXX_COMPILER_ID: ${CMAKE_CXX_COMPILER_ID}")
  message(STATUS "MSVC_VERSION: ${MSVC_VERSION}")
  message(STATUS "MSVC_TOOLSET_VERSION: ${MSVC_TOOLSET_VERSION}")
  
  if(MSVC)
    message(STATUS "CMAKE_CXX_FLAGS: ${CMAKE_CXX_FLAGS}")
    message(STATUS "CMAKE_CXX_FLAGS_DEBUG: ${CMAKE_CXX_FLAGS_DEBUG}")
    message(STATUS "CMAKE_CXX_FLAGS_RELEASE: ${CMAKE_CXX_FLAGS_RELEASE}")
    message(STATUS "CMAKE_EXE_LINKER_FLAGS: ${CMAKE_EXE_LINKER_FLAGS}")
    message(STATUS "CMAKE_SHARED_LINKER_FLAGS: ${CMAKE_SHARED_LINKER_FLAGS}")
    message(STATUS "CMAKE_MSVC_RUNTIME_LIBRARY: ${CMAKE_MSVC_RUNTIME_LIBRARY}")
    
    get_target_property(ADDON_COMPILE_OPTIONS ${ADDON_ID} COMPILE_OPTIONS)
    message(STATUS "Target COMPILE_OPTIONS: ${ADDON_COMPILE_OPTIONS}")
    
    get_target_property(ADDON_COMPILE_DEFS ${ADDON_ID} COMPILE_DEFINITIONS)
    message(STATUS "Target COMPILE_DEFINITIONS: ${ADDON_COMPILE_DEFS}")
    
    get_target_property(ADDON_LINK_OPTIONS ${ADDON_ID} LINK_OPTIONS)
    message(STATUS "Target LINK_OPTIONS: ${ADDON_LINK_OPTIONS}")
    
    get_target_property(ADDON_LINK_LIBS ${ADDON_ID} LINK_LIBRARIES)
    message(STATUS "Target LINK_LIBRARIES: ${ADDON_LINK_LIBS}")
    
    get_target_property(ADDON_MSVC_RUNTIME ${ADDON_ID} MSVC_RUNTIME_LIBRARY)
    message(STATUS "Target MSVC_RUNTIME_LIBRARY: ${ADDON_MSVC_RUNTIME}")
  endif()
  message(STATUS "=== END DEBUG ===")

  if(APPLE)
    set_target_properties(${ADDON_ID} PROPERTIES SUFFIX ".dylib")
  elseif(WIN32)
    set_target_properties(${ADDON_ID} PROPERTIES SUFFIX ".dll")
  elseif(ANDROID)
    set_target_properties(${ADDON_ID} PROPERTIES PREFIX "lib" SUFFIX ".so")
  else()
    set_target_properties(${ADDON_ID} PROPERTIES SUFFIX ".so.10")
  endif()

  target_compile_definitions(${ADDON_ID} PRIVATE
    ADDON_ID="${ADDON_ID}"
    ADDON_NAME="${ADDON_NAME}"
    ADDON_VERSION="${ADDON_VERSION}"
  )

  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/${ADDON_ID}/addon.xml.in"
    "${CMAKE_CURRENT_BINARY_DIR}/addon.xml"
    @ONLY
  )

  install(TARGETS ${ADDON_ID}
    DESTINATION "${ADDON_ID}"
  )

  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/addon.xml"
    DESTINATION "${ADDON_ID}"
  )

  install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${ADDON_ID}/resources"
    DESTINATION "${ADDON_ID}"
  )

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${ADDON_ID}/icon.png")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/${ADDON_ID}/icon.png"
      DESTINATION "${ADDON_ID}"
    )
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${ADDON_ID}/fanart.jpg")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/${ADDON_ID}/fanart.jpg"
      DESTINATION "${ADDON_ID}"
    )
  endif()
endif()
