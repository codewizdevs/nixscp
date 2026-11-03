# NixSCP for Android: the touch interface (Qt Quick) on top of nixscpcore.
# Added by the main CMakeLists.txt when ANDROID (its own directory, so that
# Qt looks for QML here only); see packaging/android/README.md.

qt_policy(SET QTP0001 NEW)
qt_standard_project_setup(REQUIRES 6.8)

file(GLOB MOBILE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
file(GLOB MOBILE_QML RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/qml/*.qml)
file(GLOB MOBILE_ICONS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/icons/*.svg)

# all at the module root (qrc:/qt/qml/NixSCP/): the pages find each other and the icon
set_source_files_properties(${CMAKE_SOURCE_DIR}/resources/appicon/nixscp-128.png PROPERTIES QT_RESOURCE_ALIAS nixscp-128.png)
foreach(QML_FILE ${MOBILE_QML})
  get_filename_component(QML_NAME ${QML_FILE} NAME)
  set_source_files_properties(${QML_FILE} PROPERTIES QT_RESOURCE_ALIAS ${QML_NAME})
endforeach()
qt_add_executable(nixscp-mobile ${MOBILE_SOURCES})
qt_add_qml_module(nixscp-mobile
  URI NixSCP
  VERSION 1.0
  QML_FILES ${MOBILE_QML}
  RESOURCES ${CMAKE_SOURCE_DIR}/resources/appicon/nixscp-128.png ${MOBILE_ICONS})

target_link_libraries(nixscp-mobile PRIVATE nixscpcore Qt6::Quick Qt6::QuickControls2 Qt6::Svg)
target_compile_definitions(nixscp-mobile PRIVATE PROJECT_VERSION="${PROJECT_VERSION}")
target_compile_options(nixscp-mobile PRIVATE -Wall -Wextra)

set_target_properties(nixscp-mobile PROPERTIES
  QT_QML_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR}/qml
  OUTPUT_NAME NixSCP
  QT_ANDROID_PACKAGE_NAME com.nixscp.android
  QT_ANDROID_APP_NAME "NixSCP"
  QT_ANDROID_VERSION_NAME ${PROJECT_VERSION}
  QT_ANDROID_VERSION_CODE 1
  QT_ANDROID_MIN_SDK_VERSION 28
  QT_ANDROID_TARGET_SDK_VERSION 35
  QT_ANDROID_PACKAGE_SOURCE_DIR ${CMAKE_SOURCE_DIR}/packaging/android/package
  # OpenSSL serves libssh and Qt's TLS (cloud sync); bundled in the APK
  QT_ANDROID_EXTRA_LIBS "${NIXSCP_ANDROID_DEPS}/lib/libssh.so;${NIXSCP_ANDROID_DEPS}/lib/libcrypto.so;${NIXSCP_ANDROID_DEPS}/lib/libssl.so")
