# 复用依赖准备脚本安装的 Microsoft.Web.WebView2 SDK，导出 WebView2::LoaderStatic。
# 版本与校验由 tools/setup/dep_3rdparty.py 单点维护，本模块不再自行下载。

set(WEBVIEW2_SDK_DIR "" CACHE PATH "Extracted Microsoft.Web.WebView2 NuGet package root")

if(NOT WEBVIEW2_SDK_DIR)
    set(WEBVIEW2_SDK_DIR "$ENV{WEBVIEW2_SDK_DIR}")
endif()
if(NOT WEBVIEW2_SDK_DIR)
    set(_webview2_default_dir "${CMAKE_CURRENT_LIST_DIR}/../../agent/cpp-algo/3rdparty/webview2")
    if(EXISTS "${_webview2_default_dir}/build/native/include/WebView2.h")
        get_filename_component(WEBVIEW2_SDK_DIR "${_webview2_default_dir}" ABSOLUTE)
    endif()
endif()
if(NOT WEBVIEW2_SDK_DIR)
    message(FATAL_ERROR
        "WebView2 SDK not found.\n"
        "Run `uv run -m tools.setup.dep_3rdparty --webview2`, or pass "
        "-DWEBVIEW2_SDK_DIR=<extracted NuGet root> / set WEBVIEW2_SDK_DIR.")
endif()

string(TOLOWER "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}" _webview2_arch)
if(_webview2_arch STREQUAL "amd64" OR _webview2_arch STREQUAL "x86_64")
    set(_webview2_arch "x64")
endif()

set(_webview2_root "${WEBVIEW2_SDK_DIR}")
set(_webview2_include "${_webview2_root}/build/native/include")
set(_webview2_lib "${_webview2_root}/build/native/${_webview2_arch}/WebView2LoaderStatic.lib")

foreach(_webview2_file "${_webview2_include}/WebView2.h" "${_webview2_lib}")
    if(NOT EXISTS "${_webview2_file}")
        message(FATAL_ERROR "WebView2 SDK file not found: ${_webview2_file}")
    endif()
endforeach()

add_library(WebView2::LoaderStatic STATIC IMPORTED GLOBAL)
set_target_properties(WebView2::LoaderStatic PROPERTIES
    IMPORTED_LOCATION "${_webview2_lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${_webview2_include}")

message(STATUS "WebView2 SDK (arch=${_webview2_arch}): ${_webview2_root}")
