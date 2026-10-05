cmake_minimum_required(VERSION 3.20)
project(dk2ml VERSION 1.0.1 LANGUAGES C CXX ASM_MASM RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# static CRT: no VC++ redistributable needed
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Door Kickers 2 is 64-bit; configure with an x64 toolchain")
endif()

# the only place the version is set; DK2ML_VERSION (Loader.h) and both DLLs' version resources come from it
configure_file(loader/Version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/dk2ml_version.h @ONLY)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/generated)
enable_testing()

add_library(minhook STATIC
    tools/third_party/minhook/src/buffer.c
    tools/third_party/minhook/src/hook.c
    tools/third_party/minhook/src/trampoline.c
    tools/third_party/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC tools/third_party/minhook/include)

# The game imports dbghelp.dll by name, so the loader is two files: dbghelp.dll (dk2ml_proxy), a stub that forwards
# every export to System32 and loads dk2ml.dll from its own folder, and dk2ml.dll, the loader. Neither links
# dbghelp.lib (it would import the stub). Exports.asm/.def and ExportNames.inc come from tools/gen_exports.ps1.
add_library(dk2ml_proxy SHARED
    loader/proxy/ProxyMain.cpp
    loader/proxy/RealDbgHelp.cpp
    loader/proxy/Exports.asm
    loader/proxy/Exports.def
    loader/proxy/ProxyVersion.rc)
set_target_properties(dk2ml_proxy PROPERTIES OUTPUT_NAME dbghelp)
target_include_directories(dk2ml_proxy PRIVATE include loader)
target_compile_definitions(dk2ml_proxy PRIVATE UNICODE _UNICODE)

add_library(dk2ml SHARED
    # startup, log, PDB lookups
    loader/core/DllMain.cpp
    loader/core/Log.cpp
    loader/core/Symbols.cpp
    # System32's dbghelp, for the PDB lookups
    loader/proxy/RealDbgHelp.cpp
    # register-preserving hooks
    loader/hooks/SafeHook.cpp
    loader/hooks/SafeHookEntry.asm
    # plugins and the API
    loader/plugins/Plugins.cpp
    loader/plugins/OptionCheck.cpp
    loader/plugins/Events.cpp
    loader/plugins/Interfaces.cpp
    loader/plugins/GameHooks.cpp
    # the game's GUI: the loader's button and screen, the GUI kit
    loader/gui/GameUi.cpp
    loader/gui/nativemods/NativeModsButton.cpp
    loader/gui/GuiKit.cpp
    loader/gui/GuiThunks.cpp
    loader/gui/nativemods/NativeModsScreen.cpp
    loader/gui/nativemods/NativeModsScreenXml.cpp
    # Workshop consent, crash reports
    loader/safety/Consent.cpp
    loader/safety/CrashReport.cpp
    loader/Version.rc)
target_include_directories(dk2ml PRIVATE include loader)
target_link_libraries(dk2ml PRIVATE minhook psapi shell32 ole32 user32 bcrypt)
target_compile_definitions(dk2ml PRIVATE UNICODE _UNICODE)

# PDB explorer and plugin dry run (shipped in the template zip's tools\)
add_executable(symtest tools/SymTest.cpp loader/core/Symbols.cpp loader/core/Log.cpp loader/proxy/RealDbgHelp.cpp
    loader/gui/GameUi.cpp loader/plugins/OptionCheck.cpp loader/plugins/Events.cpp loader/plugins/Interfaces.cpp
    loader/safety/Consent.cpp)
target_include_directories(symtest PRIVATE include loader)
target_link_libraries(symtest PRIVATE psapi bcrypt shell32 user32)
target_compile_definitions(symtest PRIVATE UNICODE _UNICODE)

# safe hooks preserve every register
add_executable(hooktest tests/hooktest/HookTest.cpp tests/hooktest/Harness.asm
    loader/hooks/SafeHook.cpp loader/hooks/SafeHookEntry.asm loader/core/Log.cpp)
target_include_directories(hooktest PRIVATE include loader)
target_link_libraries(hooktest PRIVATE minhook)
target_compile_definitions(hooktest PRIVATE UNICODE _UNICODE)

# the C example shipped to modders: keeps it compiling; consenttest uses it as a real plugin DLL
add_library(example_plugin SHARED docs/ExamplePlugin.cpp)
target_include_directories(example_plugin PRIVATE include)

# Consent.cpp: classification, fingerprints, remembered answers, re-asking on changes, manifests
add_executable(consenttest tests/ConsentTest.cpp loader/safety/Consent.cpp loader/core/Log.cpp)
target_include_directories(consenttest PRIVATE include loader)
target_link_libraries(consenttest PRIVATE bcrypt user32 shell32)
target_compile_definitions(consenttest PRIVATE UNICODE _UNICODE)
add_dependencies(consenttest example_plugin manifest_plugin nomanifest_plugin)

# manifest reader inputs: a hostile manifest (future API, unterminated and dirty texts), and none
add_library(manifest_plugin SHARED tests/ManifestPlugin.cpp)
target_include_directories(manifest_plugin PRIVATE include)
add_library(nomanifest_plugin SHARED tests/ManifestPlugin.cpp)
target_include_directories(nomanifest_plugin PRIVATE include)
target_compile_definitions(nomanifest_plugin PRIVATE DK2ML_TEST_NO_MANIFEST)

# the generated Modloader screen XML: well-formed, escaped, unique names/events
add_executable(screentest tests/ScreenTest.cpp loader/gui/nativemods/NativeModsScreenXml.cpp)
target_include_directories(screentest PRIVATE include loader)

# template/, built so it keeps compiling against the current headers
add_library(template_plugin SHARED template/src/Plugin.cpp)
target_include_directories(template_plugin PRIVATE include)
target_compile_definitions(template_plugin PRIVATE UNICODE _UNICODE)
target_link_libraries(template_plugin PRIVATE user32)

# include/dk2ml.hpp against a fake API
add_executable(hpptest tests/HppTest.cpp)
target_include_directories(hpptest PRIVATE include)

# events (subscriptions, order, crash containment, state changes), tasks and interfaces, without the game
add_executable(eventstest tests/EventsTest.cpp loader/plugins/Events.cpp loader/plugins/Interfaces.cpp loader/core/Log.cpp)
target_include_directories(eventstest PRIVATE include loader)
target_compile_definitions(eventstest PRIVATE UNICODE _UNICODE)

# GUI callback thunks (crash containment, sharing, input capture) with fake actions
add_executable(guitest tests/GuiTest.cpp loader/gui/GuiThunks.cpp loader/core/Log.cpp)
target_include_directories(guitest PRIVATE include loader)
target_compile_definitions(guitest PRIVATE UNICODE _UNICODE)

# crash report (stack walk through post hooks, text, writing once) on this exe's own code
add_executable(crashtest tests/CrashTest.cpp loader/safety/CrashReport.cpp loader/hooks/SafeHook.cpp
    loader/hooks/SafeHookEntry.asm loader/core/Log.cpp)
target_include_directories(crashtest PRIVATE include loader)
target_link_libraries(crashtest PRIVATE minhook)
target_compile_definitions(crashtest PRIVATE UNICODE _UNICODE)

# manual in-game crash-report test (Ctrl+Shift+F12): never shipped and git-ignored, so built only where it exists
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CrashPlugin.cpp)
    add_library(crash_plugin SHARED tests/CrashPlugin.cpp)
    target_include_directories(crash_plugin PRIVATE include)
    target_compile_definitions(crash_plugin PRIVATE UNICODE _UNICODE)
    target_link_libraries(crash_plugin PRIVATE user32)
endif()

# ctest --test-dir build runs these; symtest needs the game
foreach(test hooktest consenttest screentest hpptest eventstest crashtest guitest)
    add_test(NAME ${test} COMMAND ${test} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endforeach()
