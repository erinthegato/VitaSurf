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

# Show which build is installed (first 7 characters of the commit).
set(BUILD_ID "dev")
if(DEFINED ENV{GITHUB_SHA})
  string(SUBSTRING "$ENV{GITHUB_SHA}" 0 7 BUILD_ID)
endif()
add_definitions(-DBUILD_ID="${BUILD_ID}")

# vita2d needs sceSharedFb* functions. Their stub library name differs
# between VitaSDK versions, so find whichever stub actually contains them.
execute_process(
  COMMAND sh -c "for f in $VITASDK/arm-vita-eabi/lib/*_stub.a; do arm-vita-eabi-nm \"$f\" 2>/dev/null | grep -q 'T sceSharedFbBegin' && basename \"$f\" .a | sed 's/^lib//' && break; done"
  OUTPUT_VARIABLE SHAREDFB_STUB
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT SHAREDFB_STUB)
  set(SHAREDFB_STUB SceShellSvc_stub)
endif()
message(STATUS "sceSharedFb stub library: ${SHAREDFB_STUB}")

add_executable(${PROJECT_NAME}.elf
  src/main.c
  src/html.c
  src/net.c
  src/json.c
  src/github.c
)

target_link_libraries(${PROJECT_NAME}.elf
  vita2d curl ssl crypto zstd
  png jpeg freetype z pthread m c
  ${SHAREDFB_STUB} SceAppMgr_stub SceLibKernel_stub
  SceDisplay_stub SceGxm_stub SceSysmodule_stub SceCtrl_stub SceTouch_stub
  ScePgf_stub ScePvf_stub SceCommonDialog_stub SceNet_stub SceNetCtl_stub
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME}.elf)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE cacert.pem cacert.pem
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/pic0.png sce_sys/pic0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
