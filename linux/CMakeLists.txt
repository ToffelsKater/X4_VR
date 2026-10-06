# Native Linux build (docs/LINUX_PORT_PLAN.md), selected with -DX4VR_LINUX=ON from the top-level
# CMakeLists.txt. Dependencies come from the system (Nix); nothing is downloaded.
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
set(OPENVR_SOURCE_DIR "" CACHE PATH "OpenVR SDK source tree (Valve's openvr repository)")
if(NOT EXISTS "${OPENVR_SOURCE_DIR}/src/openvr_api_public.cpp")
  message(FATAL_ERROR "Set OPENVR_SOURCE_DIR to an OpenVR source tree (nix/package.nix passes nixpkgs' openvr.src).")
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

# The mod: Vulkan layer, VR runtime and OpenTrack pose sender in one library, loaded into X4 by
# the Vulkan loader (x4vr-run). Its own copy of the C++ runtime, nothing exported but the layer's
# entry points, no unresolved symbols.
add_library(x4vr_mod SHARED ${X4VR_ROOT}/src/linux/vr_layer.cpp ${X4VR_ROOT}/src/linux/runtime_bootstrap.cpp
  ${X4VR_ROOT}/src/linux/pose_sender.cpp ${X4VR_ROOT}/src/linux/x11_cursor.cpp
  ${X4VR_ROOT}/src/linux/head_watch.cpp ${X4VR_ROOT}/src/eye_targets.cpp)
set_target_properties(x4vr_mod PROPERTIES OUTPUT_NAME x4vr CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(x4vr_mod PRIVATE ${X4VR_ROOT}/src/linux)
target_link_libraries(x4vr_mod PRIVATE x4vr_vr x4vr_elf Threads::Threads)
target_compile_options(x4vr_mod PRIVATE ${X4VR_WARNINGS})
target_link_options(x4vr_mod PRIVATE -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL -Wl,-z,defs
  -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/x4vr.map)
set_target_properties(x4vr_mod PROPERTIES LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/x4vr.map)

# Phase 0 probes (docs/LINUX_PHASE0.md): the socket probe is preloaded into the whole Steam launch
# chain, so it is C with nothing but libc.
add_library(x4vr_probe SHARED ${X4VR_ROOT}/src/linux/probe_preload.c)
set_target_properties(x4vr_probe PROPERTIES C_VISIBILITY_PRESET hidden)
target_compile_options(x4vr_probe PRIVATE ${X4VR_WARNINGS})
target_link_options(x4vr_probe PRIVATE -Wl,--exclude-libs,ALL -Wl,-z,defs)

add_library(VkLayer_x4vr_probe SHARED ${X4VR_ROOT}/src/linux/probe_layer.cpp)
target_include_directories(VkLayer_x4vr_probe PRIVATE ${VULKAN_HEADERS_DIR})
target_compile_definitions(VkLayer_x4vr_probe PRIVATE VK_NO_PROTOTYPES)
set_target_properties(VkLayer_x4vr_probe PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_compile_options(VkLayer_x4vr_probe PRIVATE ${X4VR_WARNINGS})
target_link_options(VkLayer_x4vr_probe PRIVATE -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL -Wl,-z,defs)

# Layer manifests: for the installed package, and for tests in the build tree.
foreach(layer IN ITEMS x4vr x4vr_probe)
  if(layer STREQUAL "x4vr")
    set(file libx4vr.so)
  else()
    set(file libVkLayer_x4vr_probe.so)
  endif()
  set(X4VR_LAYER_PATH ${CMAKE_INSTALL_FULL_LIBDIR}/${file})
  configure_file(VkLayer_${layer}.json.in ${CMAKE_CURRENT_BINARY_DIR}/install/VkLayer_${layer}.json @ONLY)
  set(X4VR_LAYER_PATH ${CMAKE_CURRENT_BINARY_DIR}/${file})
  configure_file(VkLayer_${layer}.json.in ${CMAKE_CURRENT_BINARY_DIR}/layer/VkLayer_${layer}.json @ONLY)
endforeach()
configure_file(x4vr-run.in ${CMAKE_CURRENT_BINARY_DIR}/x4vr-run @ONLY)
configure_file(x4vr-probe-run.in ${CMAKE_CURRENT_BINARY_DIR}/x4vr-probe-run @ONLY)

install(TARGETS x4vr RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS x4vr_mod x4vr_probe VkLayer_x4vr_probe LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/install/VkLayer_x4vr.json ${CMAKE_CURRENT_BINARY_DIR}/install/VkLayer_x4vr_probe.json
        DESTINATION ${CMAKE_INSTALL_DATADIR}/vulkan/explicit_layer.d)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/x4vr-run ${CMAKE_CURRENT_BINARY_DIR}/x4vr-probe-run DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${X4VR_ROOT}/config/linux/stereo.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/x4vr)

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

  add_executable(probe_socket_fixture ${X4VR_ROOT}/tests/linux/probe_socket_fixture.cpp)
  target_include_directories(probe_socket_fixture PRIVATE ${X4VR_ROOT}/src/linux)
  target_compile_options(probe_socket_fixture PRIVATE ${X4VR_WARNINGS})
  add_test(NAME probe_preload COMMAND ${CMAKE_COMMAND} -DFIXTURE=$<TARGET_FILE:probe_socket_fixture>
           -DPROBE=$<TARGET_FILE:x4vr_probe> -DDIR=${CMAKE_CURRENT_BINARY_DIR}/probe-test
           -P ${X4VR_ROOT}/tests/linux/probe_preload_test.cmake)
endif()
