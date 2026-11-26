cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "VITASDK is not set")
  endif()
endif()

project(VitaSurf C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "VitaSurf")
set(VITA_TITLEID  "VSRF00001")
set(VITA_VERSION  "01.00")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O2 -std=gnu99")

add_executable(${PROJECT_NAME}.elf
  src/main.c
  src/html.c
  src/net.c
)

target_link_libraries(${PROJECT_NAME}.elf
  vita2d curl ssl crypto zstd
  png jpeg freetype z pthread m c
  SceSharedFb_stub SceAppMgr_stub
  SceDisplay_stub SceGxm_stub SceSysmodule_stub SceCtrl_stub SceTouch_stub
  ScePgf_stub ScePvf_stub SceCommonDialog_stub SceNet_stub SceNetCtl_stub
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME}.elf)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE cacert.pem cacert.pem
)
