# CMake build for vita-memio, the PS Vita storage plugin (firmware 3.60-3.74,
# taiHEN/HENkaku).
#
# Two artifacts:
#   vitamemio_kernel.skprx - kernel module: the faster storage path (staging),
#                               settings, the HUD, the boot guard, and the
#                               user-callable SceVitaMemio syscall library.
#   vitamemio.suprx        - user module, listed under *NPXS10015 (the
#                               Settings app: the vita-memio page under Devices).
#
# BUILD REQUIREMENTS: the VitaSDK toolchain (https://vitasdk.org) and taiHEN's
# headers/stubs (taihen):
#
#   export VITASDK=/usr/local/vitasdk
#   cmake -S . -B build \
#         -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
#   cmake --build build
#
# Order matters: the kernel module's stub library (SceVitaMemio_stub) is
# generated from vitamemio_kernel.yml and must exist before the user module
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

project(vitamemio C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_STANDARD 99)

# Which build (VM_BUILD_STR): the commit, "-dirty" with local changes. The
# Makefile passes it (-DVM_GIT_HASH=...); otherwise it is read here, when CMake
# configures (a fresh configure always has the right one).
if(NOT VM_GIT_HASH)
  execute_process(COMMAND git -c safe.directory=${CMAKE_SOURCE_DIR} rev-parse --short=7 HEAD
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE VM_GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE
                  RESULT_VARIABLE VM_GIT_RC ERROR_QUIET)
  if(VM_GIT_RC EQUAL 0 AND VM_GIT_HASH)
    execute_process(COMMAND git -c safe.directory=${CMAKE_SOURCE_DIR} diff --quiet HEAD
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                    RESULT_VARIABLE VM_GIT_DIRTY ERROR_QUIET)
    if(NOT VM_GIT_DIRTY EQUAL 0)
      set(VM_GIT_HASH "${VM_GIT_HASH}-dirty")
    endif()
  else()
    set(VM_GIT_HASH "")
  endif()
endif()
# DIAGNOSTIC (branch ncdirect): count hardware interrupts per core.
add_definitions(-DVM_INTRDIAG)

if(VM_GIT_HASH)
  add_definitions(-DVM_BUILD_SUFFIX=\"+${VM_GIT_HASH}\")
  message(STATUS "vita-memio build +${VM_GIT_HASH}")
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter -O2)

# --------------------------------------------------------------------------
# Kernel module: vitamemio_kernel.skprx
# --------------------------------------------------------------------------
add_executable(vitamemio_kernel
  kernel/main.c
  kernel/klog.c
  kernel/settings.c
  kernel/stage.c
  kernel/hud.c
  kernel/hud_i18n.c
  kernel/intrdiag.c             # DIAGNOSTIC (branch ncdirect)
)

target_link_libraries(vitamemio_kernel
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
target_link_options(vitamemio_kernel PRIVATE -nostartfiles)

vita_create_self(vitamemio_kernel.skprx vitamemio_kernel
  CONFIG ${CMAKE_SOURCE_DIR}/vitamemio_kernel.yml
  UNSAFE)
vita_create_stubs(vitamemio-stubs vitamemio_kernel
  ${CMAKE_SOURCE_DIR}/vitamemio_kernel.yml KERNEL)

# --------------------------------------------------------------------------
# User module: vitamemio.suprx (the Settings app)
# --------------------------------------------------------------------------
#
# LINK RECIPE (why weak stubs): a STRONG import of a user
# library that is not loaded in the host process makes taiHEN fail the whole
# module load SILENTLY - module_start never runs. Every standard system library
# is therefore imported as its *_stub_weak variant: a weak import that cannot
# be resolved is skipped, and when the library IS present it resolves normally.
# Only taihen stays STRONG: without it the module must not load at all. Our
# kernel syscall library is weak too, so the Settings page can say when the
# kernel module is missing.
add_executable(vitamemio
  user/main.c
  settings_app/vitamemio_settings.c  # Settings-app hooks (NPXS10015 only)
  settings_app/vitamemio_i18n.c
  settings_app/vmlog.c
)

add_dependencies(vitamemio vitamemio-stubs)

target_link_libraries(vitamemio
  # STRONG - must be present, fail loudly if not:
  taihen_stub
  # WEAK - our kernel library (without it the page still appears and says the
  # kernel module is not loaded), then the host-resident libs:
  ${CMAKE_BINARY_DIR}/libvitamemio_kernel_stub_weak.a   # SceVitaMemio syscalls
  SceLibKernel_stub_weak        # sceKernel* / sceIo* helpers
  SceIofilemgr_stub_weak        # sceIoOpen/Write/Close (the log file)
  SceProcessmgr_stub_weak       # sceKernelGetProcessTimeWide (log stamps)
  SceRegistryMgr_stub_weak      # sceRegMgrGetKeyInt (system language)
  SceAppUtil_stub_weak          # sceAppUtilSystemParamGetInt (language fallback)
)

# -Tdata: vita-elf-create appends ~1.5 KB of module info to the end of the text
# segment; a fixed data address leaves room regardless.
target_link_options(vitamemio PRIVATE -nostartfiles -Wl,-Tdata=0x81100000)

# Size-optimise the Settings-page UI code (not timing-sensitive).
set_source_files_properties(settings_app/vitamemio_settings.c
  settings_app/vitamemio_i18n.c PROPERTIES COMPILE_OPTIONS -Os)

vita_create_self(vitamemio.suprx vitamemio
  CONFIG ${CMAKE_SOURCE_DIR}/vitamemio.yml
  UNSAFE)

# --------------------------------------------------------------------------
# Tester app: vitamemio_tester.vpk (a normal application; install with VitaShell)
# --------------------------------------------------------------------------
# Runs storage tests in the plugin state the user set in Settings (it never changes
# a plugin setting itself), appends the results with that state to
# ur0:data/vita-memio/results.txt and can clear them. It reads the plugin state
# through the kernel module's SceVitaMemio library (weak import: without the
# module the state shows as unknown). Disable with -DVM_BUILD_TESTER=OFF.
option(VM_BUILD_TESTER "Build the on-device tester app (VPK)" ON)
if(VM_BUILD_TESTER)
  set(TESTER_TITLE_ID "VMEMIO001")
  add_executable(vitamemio_tester
    tester/main.c
    tester/bench.c
    tester/report.c
    tester/results.c
    tester/vita/ui.c
    tester/vita/vita_env.c
  )
  add_dependencies(vitamemio_tester vitamemio-stubs)
  # WEAK import of the plugin's syscall library: when the kernel module is not
  # loaded (or not reachable from an application) the app still starts and says so
  # on screen, instead of the console refusing to launch it. An unresolved weak
  # stub just returns, and every use of the plugin is checked (vmGetState first).
  target_link_libraries(vitamemio_tester
    ${CMAKE_BINARY_DIR}/libvitamemio_kernel_stub_weak.a
    SceLibKernel_stub
    SceIofilemgr_stub
    SceProcessmgr_stub
    SceSysmem_stub
    SceCtrl_stub
    SceDisplay_stub
    SceRtc_stub
  )
  # NO custom CONFIG here: an application needs the default module info that
  # vita-elf-create makes for it, which includes sceProcessParam (the process
  # parameters). A hand-written .yml with only `main: start` drops it, and the
  # console then refuses to start the app (error C1-15232-3 on the bubble).
  #
  # vita_create_self is a macro that APPENDS each module's "-e <config>" to
  # VITA_ELF_CREATE_FLAGS in this scope, so without a reset the tester would be
  # built with the kernel module's and the plugin's .yml files ("Could not find
  # symbol 'module_start'"). Clear it for the tester only, then put it back.
  set(VM_SAVED_ELF_CREATE_FLAGS "${VITA_ELF_CREATE_FLAGS}")
  set(VITA_ELF_CREATE_FLAGS "")
  vita_create_self(vitamemio_tester.self vitamemio_tester UNSAFE)
  set(VITA_ELF_CREATE_FLAGS "${VM_SAVED_ELF_CREATE_FLAGS}")

  # The tester's bitmap font is a subset of Terminus Font (SIL OFL 1.1): its
  # license travels with it.
  set(TESTER_VPK_FILES FILE ${CMAKE_SOURCE_DIR}/include/OFL-Terminus.txt OFL-Terminus.txt)
  # The bubble and the LiveArea (drawn by tools/gen_art.py).
  set(TESTER_LA ${CMAKE_SOURCE_DIR}/tester/sce_sys)
  list(APPEND TESTER_VPK_FILES
    FILE ${TESTER_LA}/icon0.png sce_sys/icon0.png
    FILE ${TESTER_LA}/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE ${TESTER_LA}/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE ${TESTER_LA}/livearea/contents/template.xml sce_sys/livearea/contents/template.xml)
  vita_create_vpk(vitamemio_tester.vpk ${TESTER_TITLE_ID} vitamemio_tester.self
    VERSION 01.00
    NAME "vita-memio Tester"
    ${TESTER_VPK_FILES}
  )
  # vita_create_vpk already defines the target `vitamemio_tester.vpk` as part of
  # ALL, so `cmake --build` builds the VPK together with the modules. (It is a
  # target, not a file: do not list it in DEPENDS of another target.)
endif()