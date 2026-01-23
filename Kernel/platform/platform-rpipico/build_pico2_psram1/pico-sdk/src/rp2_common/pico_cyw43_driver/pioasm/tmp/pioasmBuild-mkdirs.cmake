# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/tikhon/Документы/FUZIX/Kernel/platform/platform-rpipico/build_pico2_psram1/_deps/pico_sdk-src/tools/pioasm"
  "/home/tikhon/Документы/FUZIX/Kernel/platform/platform-rpipico/build_pico2_psram1/pioasm"
  "/home/tikhon/Документы/FUZIX/Kernel/platform/platform-rpipico/build_pico2_psram1/pioasm-install"
  "/home/tikhon/Документы/FUZIX/Kernel/platform/platform-rpipico/build_pico2_psram1/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/tmp"
  "/home/tikhon/Документы/FUZIX/Kernel/platform/platform-rpipico/build_pico2_psram1/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/src/pioasmBuild-stamp"
  "/home/tikhon/Документы/FUZIX/Kernel/platform/platform-rpipico/build_pico2_psram1/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/src"
  "/home/tikhon/Документы/FUZIX/Kernel/platform/platform-rpipico/build_pico2_psram1/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/src/pioasmBuild-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/tikhon/Документы/FUZIX/Kernel/platform/platform-rpipico/build_pico2_psram1/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/src/pioasmBuild-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/tikhon/Документы/FUZIX/Kernel/platform/platform-rpipico/build_pico2_psram1/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/src/pioasmBuild-stamp${cfgdir}") # cfgdir has leading slash
endif()
