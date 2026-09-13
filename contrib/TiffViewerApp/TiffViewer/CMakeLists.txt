cmake_minimum_required(VERSION 3.20)
project(TiffViewer)

# 由外部构建脚本显式负责重新配置，避免 VS/CMake 在 Windows 中文路径下
# 触发 ZERO_CHECK / Checking Build System 时发生异常。
set(CMAKE_SUPPRESS_REGENERATION ON)

# ──────────────────────────────────────────────────────────────────────────────
# 路径配置 — 默认按当前工作区相对路径推导，也可通过 -DCC_SRC/-DCC_BUILD 覆盖
# ──────────────────────────────────────────────────────────────────────────────
get_filename_component(WORKSPACE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(REPO_PARENT    "${WORKSPACE_ROOT}/.." ABSOLUTE)

set(CC_SRC    "${REPO_PARENT}/CloudCompare_src"   CACHE PATH "Path to CloudCompare source tree")
set(CC_BUILD  "${REPO_PARENT}/CloudCompare_build" CACHE PATH "Path to CloudCompare build tree")
set(PLUGIN_WD "${WORKSPACE_ROOT}/plugins/LmiTiffBmpPlugin" CACHE PATH "Path to shared TIFF/BMP plugin sources")

get_filename_component(CC_SRC    "${CC_SRC}" ABSOLUTE)
get_filename_component(CC_BUILD  "${CC_BUILD}" ABSOLUTE)
get_filename_component(PLUGIN_WD "${PLUGIN_WD}" ABSOLUTE)

if(NOT EXISTS "${CC_SRC}/libs/CCPluginAPI/include")
    message(FATAL_ERROR "CC_SRC 无效或缺少必要头文件目录: ${CC_SRC}")
endif()
if(NOT EXISTS "${CC_SRC}/libs/CCAppCommon/include")
    message(FATAL_ERROR "CC_SRC 无效或缺少 CCAppCommon 头文件目录: ${CC_SRC}")
endif()
if(NOT EXISTS "${CC_BUILD}/libs/qCC_db")
    message(FATAL_ERROR "CC_BUILD 无效或缺少构建产物目录: ${CC_BUILD}")
endif()
if(NOT EXISTS "${PLUGIN_WD}/TiffBmpPanel.cpp")
    message(FATAL_ERROR "PLUGIN_WD 无效，未找到共享源码: ${PLUGIN_WD}")
endif()

# ──────────────────────────────────────────────────────────────────────────────
# Qt6
# ──────────────────────────────────────────────────────────────────────────────
find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets Concurrent)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# ──────────────────────────────────────────────────────────────────────────────
# 头文件目录（来自 CloudCompare 源码树）
# ──────────────────────────────────────────────────────────────────────────────
include_directories(
    # App common（ccDisplaySettingsDlg）
    ${CC_SRC}/libs/CCAppCommon/include
    # 插件 API（ccMainAppInterface、CC_VIEW_ORIENTATION 等）
    ${CC_SRC}/libs/CCPluginAPI/include
    # ccStdPluginInterface（TiffBmpPanel 间接包含）
    ${CC_SRC}/libs/CCPluginStub/include
    # 数据库层（ccHObject、ccPointCloud、ccMesh 等）
    ${CC_SRC}/libs/qCC_db/include
    ${CC_SRC}/libs/qCC_db/extern/CCCoreLib/include
    # GL 窗口层（ccGLWindowInterface、ccGLWindow）
    ${CC_SRC}/libs/qCC_glWindow/include
    # IO 过滤器（ccGLUtils 等）
    ${CC_SRC}/libs/qCC_io/include
    # FBO（CC_FBO_LIB 导出头）
    ${CC_SRC}/libs/CCFbo/include
    # CCCoreLib 生成头（CCCoreLibExport.h — 在 build 目录中由 CMake 生成）
    ${CC_BUILD}/libs/qCC_db/extern/CCCoreLib/exports
    # 插件工作目录（TiffBmpPanel.h、TiffBmpLoader.h、DisplayModeDialog.h）
    ${PLUGIN_WD}
)

# ──────────────────────────────────────────────────────────────────────────────
# 源文件：TiffViewer 本身 + 插件逻辑（直接编译，不依赖插件 DLL）
# ──────────────────────────────────────────────────────────────────────────────
set(TV_SOURCES
    main.cpp
    TiffViewerWindow.cpp
    ${PLUGIN_WD}/TiffBmpPanel.cpp
    ${PLUGIN_WD}/TiffBmpLoader.cpp
    ${PLUGIN_WD}/DisplayModeDialog.cpp
)

set(TV_RESOURCES
    ${PLUGIN_WD}/LmiTiffBmpPlugin.qrc
    TiffViewer.qrc
)
if(WIN32)
    list(APPEND TV_RESOURCES TiffViewer.rc)
endif()

# ──────────────────────────────────────────────────────────────────────────────
# macOS app bundle 图标（未运行过 mac/generate_icon.py + iconutil 时不存在，可选）
# ──────────────────────────────────────────────────────────────────────────────
set(MAC_ICON_FILE "${CMAKE_CURRENT_LIST_DIR}/mac/TiffViewer.icns")
if(APPLE AND EXISTS "${MAC_ICON_FILE}")
    set_source_files_properties("${MAC_ICON_FILE}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    list(APPEND TV_RESOURCES "${MAC_ICON_FILE}")
endif()

if(WIN32)
    add_executable(TiffViewer WIN32 ${TV_SOURCES} ${TV_RESOURCES})
elseif(APPLE)
    add_executable(TiffViewer MACOSX_BUNDLE ${TV_SOURCES} ${TV_RESOURCES})
else()
    add_executable(TiffViewer ${TV_SOURCES} ${TV_RESOURCES})
endif()

# ──────────────────────────────────────────────────────────────────────────────
# 链接库
# ──────────────────────────────────────────────────────────────────────────────
if(APPLE)
    # macOS 上 CloudCompare 的构建产物是 .dylib/.a，具体落在哪个子目录取决于生成器
    # （Xcode 多配置会有 Release/ 子目录，Ninja/Unix Makefiles 单配置通常没有，
    # 也可能被 CMAKE_*_OUTPUT_DIRECTORY 之类的设置重定向到别的公共目录）。
    # 不猜目录布局，直接在整个 CC_BUILD 树下递归找 lib<name>.dylib / lib<name>.a。
    function(cc_find_lib OUT_VAR LIB_NAME)
        file(GLOB_RECURSE _cc_lib_candidates
            "${CC_BUILD}/lib${LIB_NAME}.dylib"
            "${CC_BUILD}/lib${LIB_NAME}.a"
        )
        list(LENGTH _cc_lib_candidates _cc_lib_count)
        if(_cc_lib_count EQUAL 0)
            message(FATAL_ERROR "找不到 ${LIB_NAME} 的编译产物（在 ${CC_BUILD} 下递归搜索 "
                "lib${LIB_NAME}.dylib / lib${LIB_NAME}.a 均未命中）。"
                "请确认 CC_BUILD 指向的 macOS 版 CloudCompare 已编译完成。")
        endif()
        list(GET _cc_lib_candidates 0 _cc_lib_found)
        set(${OUT_VAR} "${_cc_lib_found}" PARENT_SCOPE)
        message(STATUS "cc_find_lib: ${LIB_NAME} -> ${_cc_lib_found}")
    endfunction()

    cc_find_lib(CC_LIB_APPCOMMON  CCAppCommon)
    cc_find_lib(CC_LIB_PLUGINAPI  CCPluginAPI)
    cc_find_lib(CC_LIB_DB         QCC_DB_LIB)
    cc_find_lib(CC_LIB_GL         QCC_GL_LIB)
    cc_find_lib(CC_LIB_FBO        CC_FBO_LIB)
    cc_find_lib(CC_LIB_CORE       CCCoreLib)
    cc_find_lib(CC_LIB_PLUGINSTUB CCPluginStub)

    target_link_libraries(TiffViewer PRIVATE
        ${CC_LIB_APPCOMMON}
        ${CC_LIB_PLUGINAPI}
        ${CC_LIB_DB}
        ${CC_LIB_GL}
        ${CC_LIB_FBO}
        ${CC_LIB_CORE}
        ${CC_LIB_PLUGINSTUB}
        Qt6::Widgets
        Qt6::OpenGL
        Qt6::OpenGLWidgets
        Qt6::Concurrent
    )

    # rpath 直接从实际找到的 .dylib 路径反推目录，而不是猜测布局
    set(_cc_rpath_dirs "")
    foreach(_lib IN ITEMS ${CC_LIB_APPCOMMON} ${CC_LIB_PLUGINAPI} ${CC_LIB_DB} ${CC_LIB_GL} ${CC_LIB_FBO} ${CC_LIB_CORE} ${CC_LIB_PLUGINSTUB})
        get_filename_component(_lib_dir "${_lib}" DIRECTORY)
        list(APPEND _cc_rpath_dirs "${_lib_dir}")
    endforeach()
    list(REMOVE_DUPLICATES _cc_rpath_dirs)

    set_target_properties(TiffViewer PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_LIST_DIR}/mac/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "TiffViewer"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.lmi.tiffviewer"
        MACOSX_BUNDLE_ICON_FILE "TiffViewer.icns"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
        MACOSX_BUNDLE_COPYRIGHT "Copyright (c) 2026 LMI Technologies Inc."
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path/../Frameworks;${_cc_rpath_dirs}"
    )
    # 上面把 CC_BUILD 里的绝对路径直接写进 rpath，只适合"本机开发调试直接运行"，
    # 不是可分发的安装包（换机器路径就失效）。要打出能拷到别的 Mac 上运行的 .app，
    # 用 build_tiffviewer_mac.sh 里的 macdeployqt + install_name_tool 步骤，
    # 把这些 .dylib 实际拷进 TiffViewer.app/Contents/Frameworks 并改写 @rpath。
else()
    # Windows：直接链接 CloudCompare 预编译 DLL 的导入库 (.lib)
    target_link_libraries(TiffViewer PRIVATE
        "${CC_BUILD}/libs/CCAppCommon/Release/CCAppCommon.lib"
        "${CC_BUILD}/libs/CCPluginAPI/Release/CCPluginAPI.lib"
        "${CC_BUILD}/libs/qCC_db/Release/QCC_DB_LIB.lib"
        "${CC_BUILD}/libs/qCC_glWindow/Release/QCC_GL_LIB.lib"
        "${CC_BUILD}/libs/CCFbo/Release/CC_FBO_LIB.lib"
        "${CC_BUILD}/libs/qCC_db/extern/CCCoreLib/Release/CCCoreLib.lib"
        # CCPluginStub 是静态库（无 DLL）
        "${CC_BUILD}/libs/CCPluginStub/Release/CCPluginStub.lib"
        Qt6::Widgets
        Qt6::OpenGL
        Qt6::OpenGLWidgets
        Qt6::Concurrent
    )
endif()

# ──────────────────────────────────────────────────────────────────────────────
# MSVC：不弹 console 窗口（WIN32 可执行文件）
# ──────────────────────────────────────────────────────────────────────────────
if(MSVC)
    set_target_properties(TiffViewer PROPERTIES LINK_FLAGS "/MANIFEST:NO")
endif()
