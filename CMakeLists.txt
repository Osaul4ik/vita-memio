# CMake build for Turbo Storage, the PS Vita storage plugin (firmware 3.60-3.74,
# taiHEN/HENkaku).
#
# Two artifacts:
#   storageturbo_kernel.skprx - kernel module: the faster storage path (staging),
#                               settings, the HUD, the boot guard, and the
#                               user-callable SceStorageTurbo syscall library.
#   storageturbo.suprx        - user module, listed under *main (SceShell: the
#                               developer command server) and *NPXS10015 (the
#                               Settings app: the Turbo Storage page).
#
# BUILD REQUIREMENTS: the VitaSDK toolchain (https://vitasdk.org) and taiHEN's
# headers/stubs (taihen):
#
#   export VITASDK=/usr/local/vitasdk
#   cmake -S . -B build \
#         -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
#   cmake --build build
#
# Order matters: the kernel module's stub library (SceStorageTurbo_stub) is
# generated from storageturbo_kernel.yml and must exist before the user module
# links.

cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake"
        CACHE PATH "VitaSDK toolchain")
  else()
    message(FATAL_ERROR "Set VITASDK or pass -DCMAKE_TOOLCHAIN_FILE")
  endif()
endif()

project(storageturbo C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_STANDARD 99)
add_compile_options(-Wall -Wextra -Wno-unused-parameter -O2)

# --------------------------------------------------------------------------
# Kernel module: storageturbo_kernel.skprx
# --------------------------------------------------------------------------
add_executable(storageturbo_kernel
  kernel/main.c
  kernel/klog.c
  kernel/settings.c
  kernel/stage.c
  kernel/hud.c
  kernel/hud_i18n.c
)

target_link_libraries(storageturbo_kernel
  taihenForKernel_stub
  taihenModuleUtils_stub        # module_get_export_func (registry: HUD language)
  SceSysmemForDriver_stub       # memblocks, kernel->user copies
  SceSysclibForDriver_stub      # memcpy/memset/memcmp/snprintf for kernel
  SceThreadmgrForDriver_stub
  SceDebugForDriver_stub        # ksceDebugPrintf (kernel logger)
  SceIofilemgrForDriver_stub    # ksceIo* (log, settings, boot guard)
  SceSdifForDriver_stub         # ksceSdifGetSdContextGlobal (which port a call is for)
)
# taiHEN modules enter via module_start, not crt0's main - drop startup files.
target_link_options(storageturbo_kernel PRIVATE -nostartfiles)

vita_create_self(storageturbo_kernel.skprx storageturbo_kernel
  CONFIG ${CMAKE_SOURCE_DIR}/storageturbo_kernel.yml
  UNSAFE)
vita_create_stubs(storageturbo-stubs storageturbo_kernel
  ${CMAKE_SOURCE_DIR}/storageturbo_kernel.yml KERNEL)

# --------------------------------------------------------------------------
# User module: storageturbo.suprx (SceShell + the Settings app)
# --------------------------------------------------------------------------
#
# LINK RECIPE (why weak stubs, as in Wi-Fi Turbo): a STRONG import of a user
# library that is not loaded in the host process makes taiHEN fail the whole
# module load SILENTLY - module_start never runs. Every standard system library
# is therefore imported as its *_stub_weak variant: a weak import that cannot
# be resolved is skipped, and when the library IS present it resolves normally.
# Only taihen and our own kernel syscall library stay STRONG: without them the
# module must not load at all.
add_executable(storageturbo
  user/main.c
  user/dispatch.c                       # developer command server (SceShell only)
  settings_app/storageturbo_settings.c  # Settings-app hooks (NPXS10015 only)
  settings_app/storageturbo_i18n.c
  settings_app/tslog.c
)

add_dependencies(storageturbo storageturbo-stubs)

# Diagnostics build: lets the TCP :1340 command port change `internal` and
# `verify`. Off in normal builds, where those two can only be changed in Settings.
option(TS_DEV_ALLOW_RISKY "Allow set internal/verify over the command port" OFF)
if(TS_DEV_ALLOW_RISKY)
  target_compile_definitions(storageturbo PRIVATE TS_DEV_ALLOW_RISKY=1)
endif()

target_link_libraries(storageturbo
  # STRONG - must be present, fail loudly if not:
  taihen_stub
  ${CMAKE_BINARY_DIR}/libstorageturbo_kernel_stub.a   # SceStorageTurbo syscalls
  # WEAK - host-resident libs:
  SceNet_stub_weak              # sceNet* sockets (dispatch.c, SceShell only)
  SceLibKernel_stub_weak        # sceKernel* / sceIo* helpers
  SceKernelThreadMgr_stub_weak  # sceKernelDelayThread, threads
  SceIofilemgr_stub_weak        # sceIoOpen/Write/Close (the log file)
  SceProcessmgr_stub_weak       # sceKernelGetProcessTimeWide (log stamps)
  SceRegistryMgr_stub_weak      # sceRegMgrGetKeyInt (system language)
  SceAppUtil_stub_weak          # sceAppUtilSystemParamGetInt (language fallback)
)

# -Tdata: vita-elf-create appends ~1.5 KB of module info to the end of the text
# segment; a fixed data address leaves room regardless (see Wi-Fi Turbo).
target_link_options(storageturbo PRIVATE -nostartfiles -Wl,-Tdata=0x81100000)

# Size-optimise the Settings-page UI code (not timing-sensitive).
set_source_files_properties(settings_app/storageturbo_settings.c
  settings_app/storageturbo_i18n.c PROPERTIES COMPILE_OPTIONS -Os)

vita_create_self(storageturbo.suprx storageturbo
  CONFIG ${CMAKE_SOURCE_DIR}/storageturbo.yml
  UNSAFE)

# --------------------------------------------------------------------------
# Tester app: storageturbo_tester.vpk (a normal application; install with VitaShell)
# --------------------------------------------------------------------------
# Runs storage tests in the plugin state the user set in Settings (it never changes
# a plugin setting itself), appends the results with that state to
# ur0:data/turbostorage/results.txt and can clear them. It reads the plugin state
# through the kernel module's SceStorageTurbo library (weak import: without the
# module the state shows as unknown). Disable with -DTS_BUILD_TESTER=OFF.
option(TS_BUILD_TESTER "Build the on-device tester app (VPK)" ON)
if(TS_BUILD_TESTER)
  set(TESTER_TITLE_ID "TURBOTST0")
  add_executable(storageturbo_tester
    tester/main.c
    tester/bench.c
    tester/report.c
    tester/results.c
    tester/vita/ui.c
    tester/vita/vita_env.c
  )
  add_dependencies(storageturbo_tester storageturbo-stubs)
  # WEAK import of the plugin's syscall library: when the kernel module is not
  # loaded (or not reachable from an application) the app still starts and says so
  # on screen, instead of the console refusing to launch it. An unresolved weak
  # stub just returns, and every use of the plugin is checked (tsGetState first).
  target_link_libraries(storageturbo_tester
    ${CMAKE_BINARY_DIR}/libstorageturbo_kernel_stub_weak.a
    SceLibKernel_stub
    SceIofilemgr_stub
    SceProcessmgr_stub
    SceSysmem_stub
    SceCtrl_stub
    SceDisplay_stub
    SceRtc_stub
  )
  # An explicit config: the entry point of an application is crt0's _start, not
  # module_start (a missing config made vita-elf-create look for module_start).
  vita_create_self(storageturbo_tester.self storageturbo_tester
    CONFIG ${CMAKE_SOURCE_DIR}/storageturbo_tester.yml
    UNSAFE)

  set(TESTER_VPK_FILES "")
  if(EXISTS "${CMAKE_SOURCE_DIR}/tester/sce_sys/icon0.png")
    list(APPEND TESTER_VPK_FILES FILE ${CMAKE_SOURCE_DIR}/tester/sce_sys/icon0.png sce_sys/icon0.png)
  endif()
  vita_create_vpk(storageturbo_tester.vpk ${TESTER_TITLE_ID} storageturbo_tester.self
    VERSION 01.00
    NAME "Turbo Storage Tester"
    ${TESTER_VPK_FILES}
  )
  # vita_create_vpk already defines the target `storageturbo_tester.vpk` as part of
  # ALL, so `cmake --build` builds the VPK together with the modules. (It is a
  # target, not a file: do not list it in DEPENDS of another target.)
endif()