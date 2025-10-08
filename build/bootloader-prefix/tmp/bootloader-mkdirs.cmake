# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/trieunguyen0406/esp-idf/components/bootloader/subproject"
  "/home/trieunguyen0406/DATN_WorkSpace/DA2_esp/build/bootloader"
  "/home/trieunguyen0406/DATN_WorkSpace/DA2_esp/build/bootloader-prefix"
  "/home/trieunguyen0406/DATN_WorkSpace/DA2_esp/build/bootloader-prefix/tmp"
  "/home/trieunguyen0406/DATN_WorkSpace/DA2_esp/build/bootloader-prefix/src/bootloader-stamp"
  "/home/trieunguyen0406/DATN_WorkSpace/DA2_esp/build/bootloader-prefix/src"
  "/home/trieunguyen0406/DATN_WorkSpace/DA2_esp/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/trieunguyen0406/DATN_WorkSpace/DA2_esp/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/trieunguyen0406/DATN_WorkSpace/DA2_esp/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
