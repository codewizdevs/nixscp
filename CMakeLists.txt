cmake_minimum_required(VERSION 3.19)
project(nixscp VERSION 1.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(ANDROID)
  # Android: the core and the touch interface only (src/mobile)
  find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 Svg)
else()
  find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)
endif()

if(WIN32)
  # Icon and version resource of an executable (packaging/windows/nixscp.rc.in)
  function(nixscp_windows_resources TARGET DESCRIPTION)
    set(NIXSCP_TARGET ${TARGET})
    set(NIXSCP_DESCRIPTION ${DESCRIPTION})
    set(NIXSCP_ICO ${CMAKE_SOURCE_DIR}/packaging/windows/nixscp.ico)
    configure_file(${CMAKE_SOURCE_DIR}/packaging/windows/nixscp.rc.in ${CMAKE_BINARY_DIR}/${TARGET}.rc @ONLY)
    target_sources(${TARGET} PRIVATE ${CMAKE_BINARY_DIR}/${TARGET}.rc)
  endfunction()
  # windows.h first in every file, without its function-name macros (src/core/WinPrelude.h)
  add_compile_options("SHELL:-include ${CMAKE_SOURCE_DIR}/src/core/WinPrelude.h")
  add_compile_definitions(UNICODE _UNICODE)
endif()
if(ANDROID)
  # OpenSSL and libssh built by packaging/android/build-deps.sh
  set(NIXSCP_ANDROID_DEPS "${CMAKE_SOURCE_DIR}/android-sdk/deps/${ANDROID_ABI}" CACHE PATH "OpenSSL and libssh for Android")
  foreach(DEP LIBSSH:ssh LIBCRYPTO:crypto)
    string(REPLACE ":" ";" DEP ${DEP})
    list(GET DEP 0 TARGET_NAME)
    list(GET DEP 1 LIB_NAME)
    add_library(PkgConfig::${TARGET_NAME} SHARED IMPORTED)
    set_target_properties(PkgConfig::${TARGET_NAME} PROPERTIES
      IMPORTED_LOCATION "${NIXSCP_ANDROID_DEPS}/lib/lib${LIB_NAME}.so"
      INTERFACE_INCLUDE_DIRECTORIES "${NIXSCP_ANDROID_DEPS}/include")
  endforeach()
else()
  find_package(PkgConfig REQUIRED)
  # libssh 0.11+: pipelined SFTP uses the sftp_aio API
  pkg_check_modules(LIBSSH REQUIRED IMPORTED_TARGET libssh>=0.11)
  pkg_check_modules(LIBCRYPTO REQUIRED IMPORTED_TARGET libcrypto>=3.2)
endif()

# Sources are globbed so parallel work can add files without touching this file
file(GLOB CORE_SOURCES CONFIGURE_DEPENDS src/core/*.cpp src/core/*.h)
file(GLOB GUI_SOURCES CONFIGURE_DEPENDS src/gui/*.cpp src/gui/*.h)
file(GLOB CLI_SOURCES CONFIGURE_DEPENDS src/cli/*.cpp src/cli/*.h)
file(GLOB RESOURCE_FILES CONFIGURE_DEPENDS resources/*.qrc)
# WinSCP's icon set (resources/icons.qrc, resources/animations.qrc) is NOT under the
# GPL: its license (winscp-master/license.txt part B) forbids reuse in or with
# another software. It is therefore opt-in for private builds; by default NixSCP
# uses the desktop icon theme (src/gui/Icons.cpp falls back automatically).
option(NIXSCP_WINSCP_ICONS "Embed WinSCP's icon set (not redistributable, see resources/icons/README)" OFF)
if(NOT NIXSCP_WINSCP_ICONS)
  list(FILTER RESOURCE_FILES EXCLUDE REGEX "/(icons|animations)\\.qrc$")
endif()

# Core: protocols, sessions, configuration (QtCore + QtNetwork only, no GUI)
add_library(nixscpcore STATIC ${CORE_SOURCES})
target_include_directories(nixscpcore PUBLIC src)
target_link_libraries(nixscpcore PUBLIC Qt6::Core Qt6::Network PkgConfig::LIBSSH PkgConfig::LIBCRYPTO)
target_compile_options(nixscpcore PRIVATE -Wall -Wextra)

if(ANDROID)
  add_subdirectory(src/mobile)
  return()
endif()

# GUI sources, compiled once: linked into the application and the GUI tests
add_library(nixscpgui OBJECT ${GUI_SOURCES} ${RESOURCE_FILES})
target_link_libraries(nixscpgui PUBLIC nixscpcore Qt6::Widgets)
target_compile_definitions(nixscpgui PRIVATE PROJECT_VERSION="${PROJECT_VERSION}")
target_compile_options(nixscpgui PRIVATE -Wall -Wextra)

# GUI application (winscp.exe)
add_executable(nixscp src/main.cpp)
target_link_libraries(nixscp PRIVATE nixscpgui)
if(WIN32)
  # A GUI program (no console window), with icon and version information
  set_target_properties(nixscp PROPERTIES WIN32_EXECUTABLE ON)
  nixscp_windows_resources(nixscp "NixSCP")
elseif(APPLE)
  # NixSCP.app (packaging/build-macos.sh adds nixscp-cli, the icon and Qt)
  set_target_properties(nixscp PROPERTIES
    MACOSX_BUNDLE ON
    OUTPUT_NAME NixSCP
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
endif()
target_compile_definitions(nixscp PRIVATE PROJECT_VERSION="${PROJECT_VERSION}")
target_compile_options(nixscp PRIVATE -Wall -Wextra)
install(TARGETS nixscp RUNTIME DESTINATION bin BUNDLE DESTINATION .)

# Console scripting client (winscp.com)
if(CLI_SOURCES)
  add_executable(nixscp-cli ${CLI_SOURCES})
  target_link_libraries(nixscp-cli PRIVATE nixscpcore)
  if(WIN32)
    nixscp_windows_resources(nixscp-cli "NixSCP console and scripting client")
  endif()
  target_compile_definitions(nixscp-cli PRIVATE PROJECT_VERSION="${PROJECT_VERSION}")
  target_compile_options(nixscp-cli PRIVATE -Wall -Wextra)
  install(TARGETS nixscp-cli RUNTIME DESTINATION bin)
endif()

install(FILES data/nixscp.desktop DESTINATION share/applications)

# Tests: every tests/test_<name>.cpp becomes target test_<name>, linked with the core.
# Tests needing a server (test sshd etc.) skip themselves (exit 0) unless
# their environment variables are set. Tests that mention NIXSCP_GUI_TESTS are
# GUI tests: built with that define and the GUI objects, run offscreen.
option(NIXSCP_BUILD_TESTS "Build the test programs" ON)
enable_testing()
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS tests/test_*.cpp)
if(NOT NIXSCP_BUILD_TESTS)
  set(TEST_SOURCES)
elseif(WIN32)
  # Check POSIX specifics (symbolic links, modes, owners, signals, /bin/sh
  # editors, the Linux desktop) throughout, many against local test servers
  list(FILTER TEST_SOURCES EXCLUDE REGEX
    "test_(localfileops|findfiles|protocolsettings|scp|ftp|operations|serverops|sshlayer|sftp|webdav|syncex|desktopintegration|filepanel|propertiesdialog|editormanager|verify_gui|verify_desktop)\\.cpp$")
elseif(APPLE)
  # Linux desktop specifics (freedesktop.org icon themes, terminals, Trash, D-Bus)
  list(FILTER TEST_SOURCES EXCLUDE REGEX "test_verify_desktop\\.cpp$")
endif()
# test_mobile needs a POSIX system (limits, signals, /bin/sh) as the app does
if(WIN32 OR APPLE)
  list(FILTER TEST_SOURCES EXCLUDE REGEX "test_mobile\\.cpp$")
endif()
foreach(TEST_SOURCE ${TEST_SOURCES})
  get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)
  string(REPLACE "test_" "" TEST_SHORT ${TEST_NAME})
  file(STRINGS ${TEST_SOURCE} GUI_TEST_MARKER REGEX "NIXSCP_GUI_TESTS" LIMIT_COUNT 1)
  if(APPLE)
    # a home folder of its own, as the configuration is under it there
    list(APPEND TEST_SOURCE tests/MacTestHome.cpp)
  endif()
  if(GUI_TEST_MARKER)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME} PRIVATE nixscpgui)
    target_compile_definitions(${TEST_NAME} PRIVATE NIXSCP_GUI_TESTS)
    add_test(NAME ${TEST_SHORT} COMMAND ${TEST_NAME})
    set_tests_properties(${TEST_SHORT} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  else()
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME} PRIVATE nixscpcore)
    add_test(NAME ${TEST_SHORT} COMMAND ${TEST_NAME})
  endif()
  if(APPLE)
    # Qt finds libssl there, or takes Secure Transport: the same TLS as the app
    set_property(TEST ${TEST_SHORT} APPEND PROPERTY ENVIRONMENT "DYLD_LIBRARY_PATH=${LIBCRYPTO_LIBDIR}")
  endif()
endforeach()
# The Android app's logic, linked and run on the desktop (the pages themselves need a device)
if(TARGET test_mobile)
  enable_language(C)
  file(GLOB VTERM_SOURCES third_party/libvterm/src/*.c)
  add_library(vterm-test STATIC ${VTERM_SOURCES})
  target_include_directories(vterm-test PUBLIC third_party/libvterm/include PRIVATE third_party/libvterm/src)
  set_target_properties(vterm-test PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
  target_compile_options(vterm-test PRIVATE -w)
  find_package(Qt6 REQUIRED COMPONENTS Test)
  set(MOBILE_TESTED AndroidFiles AppLock FileListModel KeyStore LocalBrowser MobileQueue MobileSettings MobileUserInterface
    PrivateState RemoteSession SessionManager SiteChecker SitesModel TerminalSession TextFile)
  foreach(MOBILE_SOURCE ${MOBILE_TESTED})
    target_sources(test_mobile PRIVATE src/mobile/${MOBILE_SOURCE}.cpp src/mobile/${MOBILE_SOURCE}.h)
  endforeach()
  target_link_libraries(test_mobile PRIVATE vterm-test Qt6::Gui Qt6::Test)
  target_include_directories(test_mobile BEFORE PRIVATE tests/mobile-stubs)
  target_compile_definitions(test_mobile PRIVATE TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data")
endif()

# The WinSCP upstream tracker (tools/winscp_upstream.py, upstream/README.md), offline
find_package(Python3 COMPONENTS Interpreter QUIET)
if(NIXSCP_BUILD_TESTS AND Python3_Interpreter_FOUND AND NOT CMAKE_CROSSCOMPILING)
  add_test(NAME winscp_upstream COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tests/winscp_upstream_test.py)
endif()

# Desktop integration: AppStream metainfo and application icon.
# The icon is optional (extracted from the WinSCP sources by a separate step),
# so it is installed only when present at configure time.
include(GNUInstallDirs)
install(FILES data/nixscp.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo)
# NixSCP application icon (original artwork): every hicolor size + scalable SVG
foreach(ICON_SIZE 16 22 24 32 48 64 128 256 512)
  install(FILES resources/appicon/nixscp-${ICON_SIZE}.png
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${ICON_SIZE}x${ICON_SIZE}/apps RENAME nixscp.png)
endforeach()
install(FILES data/nixscp.svg DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps)

# Packaging (cpack -G DEB); see packaging/README.md
set(CPACK_PACKAGE_NAME nixscp)
set(CPACK_PACKAGE_VENDOR "CodeWiz")
set(CPACK_PACKAGE_CONTACT "Dragan Plavšić <hello@codewizdev.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/codewizdevs/nixscp")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "WinSCP for Linux: SFTP, SCP, FTP/FTPS, WebDAV and S3 client")
set(CPACK_PACKAGE_DESCRIPTION "NixSCP is a native Linux port of WinSCP with the WinSCP Commander\ninterface for SFTP, SCP, FTP/FTPS, WebDAV and Amazon S3 transfers, a Site\nManager compatible with WinSCP.ini and the nixscp-cli scripting client.")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_STRIP_FILES ON)
set(CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
  OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
# DEB by default; RPM with -DCPACK_GENERATOR=RPM (release builds run on the target distro)
if(NOT CPACK_GENERATOR)
  set(CPACK_GENERATOR DEB)
endif()
# Optional distro tag in package file names, e.g. -DNIXSCP_PACKAGE_SUFFIX=ubuntu26.04
set(NIXSCP_PACKAGE_SUFFIX "" CACHE STRING "Suffix appended to package file names")
# Debian / Ubuntu
set(CPACK_DEBIAN_PACKAGE_NAME nixscp)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Dragan Plavšić <hello@codewizdev.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION net)
set(CPACK_DEBIAN_PACKAGE_PRIORITY optional)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# Runtime plugins shlibdeps cannot see: SVG icon themes (Adwaita, Breeze)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "qt6-svg-plugins")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "qt6-wayland, xdg-utils, openssh-client")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/codewizdevs/nixscp")
# Fedora / openSUSE
set(CPACK_RPM_PACKAGE_NAME nixscp)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-only")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Internet")
set(CPACK_RPM_PACKAGE_URL "https://github.com/codewizdevs/nixscp")
set(CPACK_RPM_PACKAGE_AUTOREQ ON)
set(CPACK_RPM_PACKAGE_REQUIRES "qt6-qtsvg")
set(CPACK_RPM_PACKAGE_SUGGESTS "qt6-qtwayland, xdg-utils, openssh-clients")
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
  /usr/share/applications /usr/share/metainfo /usr/share/icons /usr/share/icons/hicolor
  /usr/share/icons/hicolor/scalable /usr/share/icons/hicolor/scalable/apps)
foreach(ICON_SIZE 16 22 24 32 48 64 128 256 512)
  list(APPEND CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr/share/icons/hicolor/${ICON_SIZE}x${ICON_SIZE} /usr/share/icons/hicolor/${ICON_SIZE}x${ICON_SIZE}/apps)
endforeach()
if(NIXSCP_PACKAGE_SUFFIX)
  set(CPACK_DEBIAN_FILE_NAME "nixscp_${PROJECT_VERSION}_${NIXSCP_PACKAGE_SUFFIX}_amd64.deb")
  set(CPACK_RPM_FILE_NAME "nixscp-${PROJECT_VERSION}.${NIXSCP_PACKAGE_SUFFIX}.x86_64.rpm")
else()
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
endif()
include(CPack)
