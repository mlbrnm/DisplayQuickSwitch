cmake_minimum_required(VERSION 3.24)
project(DisplayQuickSwitch VERSION 0.4.0 LANGUAGES CXX RC)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Build with the Microsoft C++ toolchain for Windows x64.")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
include(FetchContent)
# Public NVIDIA SDK, MIT licensed. No driver DLL is redistributed.
FetchContent_Declare(nvapi
  URL https://github.com/NVIDIA/nvapi/archive/70d337db9186e968eab622f7e786de7e437faf3d.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nvapi)
option(QS_ENABLE_AMD_ADLX "Enable AMD ADLX (review its SDK license and distribution/EULA requirements first)" OFF)
if(QS_ENABLE_AMD_ADLX)
  FetchContent_Declare(adlx
    URL https://github.com/GPUOpen-LibrariesAndSDKs/ADLX/archive/32b5a740d42295c5dfe9026b9f52683da0f3af91.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(adlx)
endif()
FetchContent_Declare(igcl
  URL https://github.com/intel/drivers.gpu.control-library/archive/b6c462933502e13d1537dd5024949a51be30e63d.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
# Only public headers are used; implementations are loaded from the installed drivers.
FetchContent_MakeAvailable(igcl)
add_library(qs_core STATIC src/model.cpp src/config.cpp src/windows_scaling_model.cpp src/vendor_transaction.cpp)
target_include_directories(qs_core PUBLIC src)
target_compile_definitions(qs_core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(qs_core PRIVATE /W4 /permissive- /utf-8)
add_executable(DisplayQuickSwitch WIN32 src/app.cpp src/backend.cpp src/vendor_driver.cpp src/windows_scaling.cpp src/monitor_layout.cpp src/app.rc)
target_include_directories(DisplayQuickSwitch PRIVATE "${nvapi_SOURCE_DIR}" "${igcl_SOURCE_DIR}/include")
if(QS_ENABLE_AMD_ADLX)
  target_compile_definitions(DisplayQuickSwitch PRIVATE QS_ENABLE_AMD_ADLX)
  target_include_directories(DisplayQuickSwitch PRIVATE "${adlx_SOURCE_DIR}/SDK/Include")
  add_custom_command(TARGET DisplayQuickSwitch POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${adlx_SOURCE_DIR}/ADLX SDK License Agreement.pdf"
    "$<TARGET_FILE_DIR:DisplayQuickSwitch>/AMD-ADLX-SDK-LICENSE.pdf")
endif()
target_compile_options(DisplayQuickSwitch PRIVATE /W4 /permissive- /utf-8)
target_link_options(DisplayQuickSwitch PRIVATE /MANIFEST:NO)
target_link_libraries(DisplayQuickSwitch PRIVATE qs_core
  "${nvapi_SOURCE_DIR}/amd64/nvapi64.lib" user32 shell32 comctl32 ole32 advapi32)
add_custom_command(TARGET DisplayQuickSwitch POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${nvapi_SOURCE_DIR}/License.txt"
  "$<TARGET_FILE_DIR:DisplayQuickSwitch>/NVIDIA-NVAPI-LICENSE.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${igcl_SOURCE_DIR}/LICENSE"
  "$<TARGET_FILE_DIR:DisplayQuickSwitch>/INTEL-IGCL-LICENSE.txt")
include(CTest)
if(BUILD_TESTING)
  add_executable(qs_tests tests/core_tests.cpp)
  target_link_libraries(qs_tests PRIVATE qs_core)
  target_compile_options(qs_tests PRIVATE /W4 /permissive- /utf-8)
  add_test(NAME core COMMAND qs_tests)
  add_executable(qs_backend_tests tests/backend_tests.cpp src/backend.cpp)
  target_include_directories(qs_backend_tests PRIVATE "${nvapi_SOURCE_DIR}")
  target_compile_definitions(qs_backend_tests PRIVATE QS_TEST_BACKEND)
  target_compile_options(qs_backend_tests PRIVATE /W4 /permissive- /utf-8)
  target_link_libraries(qs_backend_tests PRIVATE qs_core user32)
  add_test(NAME backend_transactions COMMAND qs_backend_tests)
  add_executable(qs_windows_scaling_tests tests/windows_scaling_tests.cpp src/windows_scaling.cpp)
  target_include_directories(qs_windows_scaling_tests PRIVATE tests)
  target_compile_definitions(qs_windows_scaling_tests PRIVATE QS_TEST_DPI)
  target_compile_options(qs_windows_scaling_tests PRIVATE /W4 /permissive- /utf-8)
  target_link_libraries(qs_windows_scaling_tests PRIVATE qs_core)
  add_test(NAME windows_scaling_api COMMAND qs_windows_scaling_tests)
  add_executable(qs_layout_tests tests/monitor_layout_tests.cpp src/monitor_layout.cpp)
  target_include_directories(qs_layout_tests PRIVATE tests)
  target_compile_definitions(qs_layout_tests PRIVATE QS_TEST_LAYOUT)
  target_compile_options(qs_layout_tests PRIVATE /W4 /permissive- /utf-8)
  target_link_libraries(qs_layout_tests PRIVATE qs_core)
  add_test(NAME monitor_layouts COMMAND qs_layout_tests)
  add_executable(qs_vendor_tests tests/vendor_tests.cpp src/vendor_driver.cpp)
  target_include_directories(qs_vendor_tests PRIVATE "${igcl_SOURCE_DIR}/include")
  target_compile_definitions(qs_vendor_tests PRIVATE QS_TEST_VENDOR)
  target_compile_options(qs_vendor_tests PRIVATE /W4 /permissive- /utf-8)
  target_link_libraries(qs_vendor_tests PRIVATE qs_core)
  add_test(NAME vendor_driver_transactions COMMAND qs_vendor_tests)
endif()
