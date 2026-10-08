# Native Linux build (README.md, docs/linux/ARCHITECTURE.md), selected with -DX4VR_LINUX=ON from the top-level
# CMakeLists.txt. Dependencies come from the system, except OpenVR's sources: fetched at configure
# time unless OPENVR_SOURCE_DIR names a copy (Nix passes nixpkgs' openvr.src).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
find_package(Threads REQUIRED)
include(GNUInstallDirs)
set(X4VR_ROOT ${PROJECT_SOURCE_DIR})

find_path(VULKAN_HEADERS_DIR vulkan/vk_layer.h REQUIRED)
# OpenVR's client library is built from source into the mod: the packaged one loads its own
# libstdc++ and jsoncpp, which could clash with the older ones in X4's Steam runtime container.
set(OPENVR_SOURCE_DIR "" CACHE PATH "OpenVR SDK source tree (Valve's openvr repository); empty: fetched")
if(NOT OPENVR_SOURCE_DIR)
  # The version nixpkgs has; SOURCE_SUBDIR names no CMakeLists.txt, so OpenVR's own build stays out.
  include(FetchContent)
  FetchContent_Declare(openvr GIT_REPOSITORY https://github.com/ValveSoftware/openvr.git GIT_TAG v2.15.6
                       GIT_SHALLOW TRUE SOURCE_SUBDIR do-not-build)
  FetchContent_MakeAvailable(openvr)
  set(OPENVR_SOURCE_DIR ${openvr_SOURCE_DIR})
endif()
if(NOT EXISTS "${OPENVR_SOURCE_DIR}/src/openvr_api_public.cpp")
  message(FATAL_ERROR "No OpenVR source tree at OPENVR_SOURCE_DIR (${OPENVR_SOURCE_DIR}).")
endif()

# -Wno-missing-field-initializers: the Windows code initialises Vulkan structs as Info{VK_STRUCTURE_TYPE_...}.
set(X4VR_WARNINGS -Wall -Wextra -Werror -Wno-missing-field-initializers)

add_library(x4vr_openvr_api STATIC
  ${OPENVR_SOURCE_DIR}/src/openvr_api_public.cpp ${OPENVR_SOURCE_DIR}/src/jsoncpp.cpp
  ${OPENVR_SOURCE_DIR}/src/vrcore/dirtools_public.cpp ${OPENVR_SOURCE_DIR}/src/vrcore/envvartools_public.cpp
  ${OPENVR_SOURCE_DIR}/src/vrcore/pathtools_public.cpp ${OPENVR_SOURCE_DIR}/src/vrcore/sharedlibtools_public.cpp
  ${OPENVR_SOURCE_DIR}/src/vrcore/hmderrors_public.cpp ${OPENVR_SOURCE_DIR}/src/vrcore/vrpathregistry_public.cpp
  ${OPENVR_SOURCE_DIR}/src/vrcore/strtools_public.cpp)
target_include_directories(x4vr_openvr_api PRIVATE ${OPENVR_SOURCE_DIR}/src ${OPENVR_SOURCE_DIR}/src/vrcore
  PUBLIC ${OPENVR_SOURCE_DIR}/headers)
target_compile_definitions(x4vr_openvr_api PRIVATE VR_API_PUBLIC VRCORE_NO_PLATFORM LINUX POSIX LINUX64)
set_target_properties(x4vr_openvr_api PROPERTIES CXX_VISIBILITY_PRESET hidden)
target_compile_options(x4vr_openvr_api PRIVATE -w) # Valve's code, as shipped

# Portable code shared with the Windows build, compiled as it is.
add_library(x4vr_vr STATIC ${X4VR_ROOT}/src/math.cpp ${X4VR_ROOT}/src/session.cpp)
target_include_directories(x4vr_vr PUBLIC ${X4VR_ROOT}/include ${VULKAN_HEADERS_DIR})
target_compile_definitions(x4vr_vr PUBLIC VK_NO_PROTOTYPES)
target_link_libraries(x4vr_vr PUBLIC x4vr_openvr_api)

add_library(x4vr_elf STATIC ${X4VR_ROOT}/src/linux/elf_classes.cpp)
target_include_directories(x4vr_elf PUBLIC ${X4VR_ROOT}/src/linux)
target_compile_options(x4vr_elf PRIVATE ${X4VR_WARNINGS})

add_executable(x4vr ${X4VR_ROOT}/tools/linux/x4vr_cli.cpp)
target_link_libraries(x4vr PRIVATE x4vr_vr x4vr_elf)
target_compile_options(x4vr PRIVATE ${X4VR_WARNINGS})
# For uninstalling: the library folder under the install prefix (lib or lib64) and the source
# folder this was built from (shown, never deleted).
target_compile_definitions(x4vr PRIVATE X4VR_INSTALL_LIBDIR="${CMAKE_INSTALL_LIBDIR}" X4VR_SOURCE_DIR="${X4VR_ROOT}")
# The version (git describe) and commit built (build_info.h), re-read at every build: for the menu,
# `x4vr version`, bug reports and the log. Without git (Nix), they come from these.
set(X4VR_GIT_COMMIT "" CACHE STRING "The commit built, when the source has no git (Nix)")
set(X4VR_GIT_DESCRIBE "" CACHE STRING "Its git describe, when the source has no git (Nix)")
add_custom_target(x4vr_build_info
  COMMAND ${CMAKE_COMMAND} -DROOT=${X4VR_ROOT} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.h
          -DGIT_COMMIT=${X4VR_GIT_COMMIT} -DGIT_DESCRIBE=${X4VR_GIT_DESCRIBE}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/build_info.cmake
  BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.h)
add_dependencies(x4vr x4vr_build_info)
target_include_directories(x4vr PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

# The mod: Vulkan layer, VR runtime and OpenTrack client in one library, loaded into X4 by
# the Vulkan loader (x4vr-run). Its own copy of the C++ runtime, nothing exported but the layer's
# entry points, no unresolved symbols.
add_library(x4vr_mod SHARED ${X4VR_ROOT}/src/linux/observe_layer.cpp ${X4VR_ROOT}/src/linux/runtime_bootstrap.cpp
  ${X4VR_ROOT}/src/linux/opentrack_client.cpp ${X4VR_ROOT}/src/linux/x11_cursor.cpp
  ${X4VR_ROOT}/src/eye_targets.cpp)
set_target_properties(x4vr_mod PROPERTIES OUTPUT_NAME x4vr CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(x4vr_mod PRIVATE ${X4VR_ROOT}/src/linux ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_dependencies(x4vr_mod x4vr_build_info)
target_link_libraries(x4vr_mod PRIVATE x4vr_vr x4vr_elf Threads::Threads)
target_compile_options(x4vr_mod PRIVATE ${X4VR_WARNINGS})
target_link_options(x4vr_mod PRIVATE -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL -Wl,-z,defs
  -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/x4vr.map)
set_target_properties(x4vr_mod PROPERTIES LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/x4vr.map)

# Layer manifests: for the installed package, and for tests in the build tree.
set(X4VR_LAYER_PATH ${CMAKE_INSTALL_FULL_LIBDIR}/libx4vr.so)
configure_file(VkLayer_x4vr.json.in ${CMAKE_CURRENT_BINARY_DIR}/install/VkLayer_x4vr.json @ONLY)
set(X4VR_LAYER_PATH ${CMAKE_CURRENT_BINARY_DIR}/libx4vr.so)
configure_file(VkLayer_x4vr.json.in ${CMAKE_CURRENT_BINARY_DIR}/layer/VkLayer_x4vr.json @ONLY)
configure_file(x4vr-run.in ${CMAKE_CURRENT_BINARY_DIR}/x4vr-run @ONLY)

install(TARGETS x4vr RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS x4vr_mod LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/install/VkLayer_x4vr.json DESTINATION ${CMAKE_INSTALL_DATADIR}/vulkan/explicit_layer.d)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/x4vr-run DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${X4VR_ROOT}/config/linux/stereo.txt ${X4VR_ROOT}/config/linux/notices.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/x4vr)
install(DIRECTORY ${X4VR_ROOT}/config/linux/profiles/ DESTINATION ${CMAKE_INSTALL_DATADIR}/x4vr/profiles)

if(BUILD_TESTING) # include(CTest) is in the top-level CMakeLists.txt, so ctest finds these
  # Existing suites from tests/ that are platform-independent, built as they are. The others need
  # Windows (MASM fixtures, PE code scanning, the crash recorder, named events) or the Windows OpenXR
  # backend (backend_tests).
  foreach(suite IN ITEMS launcher_tests x4_camera_tests eye_targets_tests camera_sampling_tests vulkan_bootstrap_tests)
    add_executable(${suite} ${X4VR_ROOT}/tests/${suite}.cpp)
    target_include_directories(${suite} PRIVATE ${X4VR_ROOT}/src)
    target_link_libraries(${suite} PRIVATE x4vr_vr)
  endforeach()
  target_sources(x4_camera_tests PRIVATE ${X4VR_ROOT}/src/x4_camera.cpp)
  target_sources(eye_targets_tests PRIVATE ${X4VR_ROOT}/src/eye_targets.cpp)
  add_test(NAME launcher_logic COMMAND launcher_tests)
  add_test(NAME x4_eye_camera COMMAND x4_camera_tests)
  add_test(NAME eye_target_lifetime COMMAND eye_targets_tests)
  add_test(NAME camera_sampling COMMAND camera_sampling_tests)
  add_test(NAME vulkan_bootstrap_contract COMMAND vulkan_bootstrap_tests)

  add_executable(opentrack_tests ${X4VR_ROOT}/tests/linux/opentrack_tests.cpp)
  target_include_directories(opentrack_tests PRIVATE ${X4VR_ROOT}/src/linux)
  target_compile_options(opentrack_tests PRIVATE ${X4VR_WARNINGS})
  add_test(NAME opentrack_packet COMMAND opentrack_tests)

  add_executable(md5_tests ${X4VR_ROOT}/tests/linux/md5_tests.cpp)
  target_include_directories(md5_tests PRIVATE ${X4VR_ROOT}/src/linux)
  target_compile_options(md5_tests PRIVATE ${X4VR_WARNINGS})
  add_test(NAME md5 COMMAND md5_tests)

  # The scan reads absolute pointers from the file, as in Linux X4: the test inspects itself, so
  # it is linked without PIE like the game.
  add_executable(elf_classes_tests ${X4VR_ROOT}/tests/linux/elf_classes_tests.cpp)
  target_link_libraries(elf_classes_tests PRIVATE x4vr_elf)
  set_target_properties(elf_classes_tests PROPERTIES POSITION_INDEPENDENT_CODE OFF)
  target_compile_options(elf_classes_tests PRIVATE ${X4VR_WARNINGS} -fno-pie)
  target_link_options(elf_classes_tests PRIVATE -no-pie)
  add_test(NAME elf_classes COMMAND elf_classes_tests)

  add_executable(code_scan_tests ${X4VR_ROOT}/tests/linux/code_scan_tests.cpp)
  target_link_libraries(code_scan_tests PRIVATE x4vr_elf)
  set_target_properties(code_scan_tests PROPERTIES POSITION_INDEPENDENT_CODE OFF)
  target_compile_options(code_scan_tests PRIVATE ${X4VR_WARNINGS} -fno-pie -O2 -fcf-protection=none) # X4: no endbr64
  target_link_options(code_scan_tests PRIVATE -no-pie)
  add_test(NAME code_scan COMMAND code_scan_tests)

  add_executable(steam_config_tests ${X4VR_ROOT}/tests/linux/steam_config_tests.cpp)
  target_include_directories(steam_config_tests PRIVATE ${X4VR_ROOT}/tools/linux)
  target_compile_options(steam_config_tests PRIVATE ${X4VR_WARNINGS})
  add_test(NAME steam_config COMMAND steam_config_tests)

  add_executable(report_info_tests ${X4VR_ROOT}/tests/linux/report_info_tests.cpp)
  target_include_directories(report_info_tests PRIVATE ${X4VR_ROOT}/tools/linux)
  target_compile_options(report_info_tests PRIVATE ${X4VR_WARNINGS})
  add_test(NAME report_info COMMAND report_info_tests)

  add_executable(vr_query_tests ${X4VR_ROOT}/tests/linux/vr_query_tests.cpp)
  target_include_directories(vr_query_tests PRIVATE ${X4VR_ROOT}/src/linux)
  target_compile_options(vr_query_tests PRIVATE ${X4VR_WARNINGS})
  add_test(NAME vr_query COMMAND vr_query_tests)

  # Issue #7: X4 hung when SteamVR answered the mod's Vulkan questions inside X4's Vulkan setup.
  # loader_lock_tests stands in for X4 and the Vulkan loader (its lock included), fake_vrclient for
  # SteamVR; no Vulkan loader or driver needed.
  add_library(fake_vrclient MODULE ${X4VR_ROOT}/tests/linux/fake_vrclient.cpp)
  target_include_directories(fake_vrclient PRIVATE ${OPENVR_SOURCE_DIR}/headers ${OPENVR_SOURCE_DIR}/src)
  target_link_libraries(fake_vrclient PRIVATE ${CMAKE_DL_LIBS})
  set_target_properties(fake_vrclient PROPERTIES PREFIX "" OUTPUT_NAME vrclient
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fake_steamvr/bin/linux64)
  add_executable(loader_lock_tests ${X4VR_ROOT}/tests/linux/loader_lock_tests.cpp)
  target_include_directories(loader_lock_tests PRIVATE ${X4VR_ROOT}/src/linux ${VULKAN_HEADERS_DIR})
  target_compile_definitions(loader_lock_tests PRIVATE VK_NO_PROTOTYPES)
  target_compile_options(loader_lock_tests PRIVATE ${X4VR_WARNINGS})
  target_link_libraries(loader_lock_tests PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  set_target_properties(loader_lock_tests PROPERTIES ENABLE_EXPORTS ON) # x4vr_test_* for fake_vrclient
  add_dependencies(loader_lock_tests x4vr_mod fake_vrclient)
  add_test(NAME loader_lock COMMAND loader_lock_tests $<TARGET_FILE:x4vr_mod> ${CMAKE_CURRENT_BINARY_DIR}/fake_steamvr)
  set_tests_properties(loader_lock PROPERTIES TIMEOUT 60)

  add_executable(state_files_tests ${X4VR_ROOT}/tests/linux/state_files_tests.cpp)
  target_include_directories(state_files_tests PRIVATE ${X4VR_ROOT}/tools/linux)
  target_compile_options(state_files_tests PRIVATE ${X4VR_WARNINGS})
  add_test(NAME state_files COMMAND state_files_tests)

  foreach(suite IN ITEMS launch_option_tests settings_swap_tests x4_settings_tests)
    add_executable(${suite} ${X4VR_ROOT}/tests/linux/${suite}.cpp)
    target_include_directories(${suite} PRIVATE ${X4VR_ROOT}/tools/linux)
    target_compile_options(${suite} PRIVATE ${X4VR_WARNINGS})
  endforeach()
  add_test(NAME launch_option COMMAND launch_option_tests)
  add_test(NAME settings_swap COMMAND settings_swap_tests)
  add_test(NAME x4_settings COMMAND x4_settings_tests)

endif()
