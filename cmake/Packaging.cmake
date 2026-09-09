if(NOT TARGET pdfeditor-desktop)
  message(FATAL_ERROR "Packaging requires FOLIOFORGE_DESKTOP=ON")
endif()
include(GNUInstallDirs)
set(FOLIOFORGE_RUNTIME_DIRS "" CACHE STRING "Extra dependency library/DLL search directories")
set(FOLIOFORGE_NOTICES_DIR "" CACHE PATH "Directory of third-party notices to include")
set(CPACK_PACKAGE_CONTACT "FolioForge maintainers" CACHE STRING "Package maintainer/contact")
set(CPACK_WIX_VERSION "3" CACHE STRING "WiX toolset major version (3 or 4)")
if(APPLE)
  set_target_properties(pdfeditor-desktop PROPERTIES
    MACOSX_BUNDLE TRUE OUTPUT_NAME FolioForge
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.quantasysom.folioforge"
    MACOSX_BUNDLE_BUNDLE_NAME FolioForge
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
  set(cli_destination "FolioForge.app/Contents/MacOS")
  set(notice_destination "FolioForge.app/Contents/Resources/notices")
  set(deploy_executable "FolioForge.app")
else()
  set(cli_destination "${CMAKE_INSTALL_BINDIR}")
  set(notice_destination "${CMAKE_INSTALL_DATADIR}/folioforge/notices")
  set(deploy_executable "${CMAKE_INSTALL_BINDIR}/$<TARGET_FILE_NAME:pdfeditor-desktop>")
endif()
if(UNIX AND NOT APPLE)
  # Runtime libraries deployed next to the application must be relocatable.
  set_target_properties(pdfeditor-desktop pdfeditor-cli PROPERTIES
    INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
install(TARGETS pdfeditor-desktop BUNDLE DESTINATION . RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS pdfeditor-cli RUNTIME DESTINATION "${cli_destination}")
qt_generate_deploy_script(TARGET pdfeditor-desktop OUTPUT_SCRIPT qt_deploy CONTENT "
qt_deploy_runtime_dependencies(
  EXECUTABLE \"${deploy_executable}\"
  ADDITIONAL_EXECUTABLES \"${cli_destination}/$<TARGET_FILE_NAME:pdfeditor-cli>\"
  GENERATE_QT_CONF NO_TRANSLATIONS)
")
install(SCRIPT "${qt_deploy}")
# windeployqt deploys Qt, not QPDF/PDFium. Resolve actual imports, not versioned DLL names.
if(WIN32)
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "${CMAKE_INSTALL_BINDIR}")
  include(InstallRequiredSystemLibraries)
  set(runtime_script "${CMAKE_CURRENT_BINARY_DIR}/deploy-native-$<CONFIG>.cmake")
  file(GENERATE OUTPUT "${runtime_script}" CONTENT "
if(POLICY CMP0207)
  cmake_policy(SET CMP0207 NEW)
endif()
set(search_dirs \"${FOLIOFORGE_RUNTIME_DIRS}\")
file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES \"$<TARGET_FILE:pdfeditor-cli>\"
  DIRECTORIES \${search_dirs}
  PRE_EXCLUDE_REGEXES \"api-ms-.*\" \"ext-ms-.*\"
  POST_EXCLUDE_REGEXES \".*[Ww][Ii][Nn][Dd][Oo][Ww][Ss]/[Ss][Yy][Ss][Tt][Ee][Mm]32/.*\"
  RESOLVED_DEPENDENCIES_VAR resolved UNRESOLVED_DEPENDENCIES_VAR unresolved)
if(unresolved)
  message(FATAL_ERROR \"Unresolved runtime dependencies: \${unresolved}\")
endif()
foreach(lib IN LISTS resolved)
  file(TO_CMAKE_PATH \"\${lib}\" normalized)
  string(TOLOWER \"\${normalized}\" normalized)
  if(normalized MATCHES \"/windows/system32/\")
    continue()
  endif()
  file(INSTALL DESTINATION \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\" TYPE SHARED_LIBRARY FILES \"\${lib}\")
endforeach()
")
  install(SCRIPT "${runtime_script}")
elseif(APPLE)
  # Fix non-Qt dylibs and the CLI inside the app after Qt's deployment step.
  install(CODE "
include(BundleUtilities)
fixup_bundle(\"\${CMAKE_INSTALL_PREFIX}/FolioForge.app\"
  \"\${CMAKE_INSTALL_PREFIX}/${cli_destination}/pdfeditor-cli\"
  \"${FOLIOFORGE_RUNTIME_DIRS}\")
")
endif()
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
  DESTINATION "${notice_destination}")
if(FOLIOFORGE_NOTICES_DIR)
  if(NOT IS_DIRECTORY "${FOLIOFORGE_NOTICES_DIR}")
    message(FATAL_ERROR "FOLIOFORGE_NOTICES_DIR is not a directory")
  endif()
  install(DIRECTORY "${FOLIOFORGE_NOTICES_DIR}/" DESTINATION "${notice_destination}/third-party")
endif()
if(UNIX AND NOT APPLE)
  install(FILES "${PROJECT_SOURCE_DIR}/packaging/folioforge.desktop" DESTINATION share/applications)
endif()
set(CPACK_PACKAGE_NAME FolioForge)
set(CPACK_PACKAGE_VENDOR QuantaSys)
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "FolioForge PDF editor and writer")
set(CPACK_PACKAGE_INSTALL_DIRECTORY FolioForge)
set(CPACK_PACKAGE_FILE_NAME "FolioForge-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
configure_file("${PROJECT_SOURCE_DIR}/LICENSE" "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt" COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt")
set(CPACK_PACKAGE_EXECUTABLES "pdfeditor-desktop" "FolioForge")
set(CPACK_WIX_UPGRADE_GUID "305D82D8-61A5-4131-8357-1806D1AD9513")
set(CPACK_WIX_PROGRAM_MENU_FOLDER FolioForge)
set(CPACK_NSIS_DISPLAY_NAME FolioForge)
set(CPACK_NSIS_MODIFY_PATH OFF)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_RPM_PACKAGE_AUTOREQPROV ON)
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
if(APPLE)
  set(CPACK_PACKAGING_INSTALL_PREFIX "/")
  set(CPACK_PACKAGE_DEFAULT_LOCATION "/Applications")
  set(CPACK_GENERATOR "DragNDrop")
elseif(WIN32)
  set(CPACK_PACKAGING_INSTALL_PREFIX "/")
  set(CPACK_GENERATOR ZIP)
else()
  set(CPACK_GENERATOR TGZ)
endif()
include(CPack)
