# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/tikhon/Документы/FUZIX/Kernel/platform/platform-rpipico/build_pico2_psram1/_deps/picotool-src"
  "/home/tikhon/Документы/FUZIX/Kernel/platform/platform-rpipico/build_pico2_psram1/_deps/picotool-build"
  "/home/tikhon/Документы/FUZIX/Kernel/platform/platform-rpipico/build_pico2_psram1/_deps"
  "/home/tikhon/Документы/FUZIX/Kernel/platform/platform-rpipico/build_pico2_psram1/pico-sdk/src/rp2350/boot_stage2/picotool/tmp"
  "/home/tikhon/Документы/FUZIX/Kernel/platform/platform-rpipico/build_pico2_psram1/pico-sdk/src/rp2350/boot_stage2/picotool/src/picotoolBuild-stamp"
  "/home/tikhon/Документы/FUZIX/Kernel/platform/platform-rpipico/build_pico2_psram1/pico-sdk/src/rp2350/boot_stage2/picotool/src"
  "/home/tikhon/Документы/FUZIX/Kernel/platform/platform-rpipico/build_pico2_psram1/pico-sdk/src/rp2350/boot_stage2/picotool/src/picotoolBuild-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/tikhon/Документы/FUZIX/Kernel/platform/platform-rpipico/build_pico2_psram1/pico-sdk/src/rp2350/boot_stage2/picotool/src/picotoolBuild-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/tikhon/Документы/FUZIX/Kernel/platform/platform-rpipico/build_pico2_psram1/pico-sdk/src/rp2350/boot_stage2/picotool/src/picotoolBuild-stamp${cfgdir}") # cfgdir has leading slash
endif()
