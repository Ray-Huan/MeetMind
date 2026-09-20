cmake_minimum_required(VERSION 3.20)

# ============================================================================
#  MeetMind — 端侧语音转写与智能会议纪要工具
#  构建目标:
#    mm_core    核心静态库（不依赖 Qt，可独立复用）
#    mm_cli     命令行外壳
#    mm_tests   测试可执行文件
#    mm_gui     Qt6 图形外壳（可选）
# ============================================================================

project(MeetMind
        VERSION 1.0.0
        DESCRIPTION "端侧语音转写与智能会议纪要工具"
        LANGUAGES CXX)

# ---------------------------------------------------------------- 全局编译选项
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ---------------------------------------------------------------- 功能开关
option(MEETMIND_BUILD_CLI    "构建命令行工具"            ON)
option(MEETMIND_BUILD_GUI    "构建 Qt 图形界面"          ON)
option(MEETMIND_BUILD_TESTS  "构建测试"                  ON)
option(MEETMIND_WITH_WHISPER "启用 whisper.cpp 端侧推理"  ON)
option(MEETMIND_WARNINGS_AS_ERRORS "把编译告警当作错误"  OFF)
# 注意：默认关闭。实测（32 逻辑核，whisper base，60 秒片段）开启 OpenMP 反而变慢：
#   j=8  13.3s → 14.1s   j=16 11.3s → 14.6s   j=24 11.2s → 16.3s
# 原因是 ggml 已用自带线程池（n_threads）并行，OpenMP 再叠一层会造成线程超额订阅互相抢核。
option(MEETMIND_GGML_OPENMP  "启用 ggml 的 OpenMP CPU 并行（实测在本负载下更慢，默认关）" OFF)
option(MEETMIND_GGML_GPU     "启用 ggml 的 GPU 后端（需 CUDA/Vulkan 工具链）" OFF)
# GPU/CUDA 后端无法用 MinGW 编译：Windows 版 nvcc 的宿主编译器只能是 MSVC（cl.exe），
# MinGW 的 gcc 不受支持。因此提供「预编译 CUDA DLL」接入路径——用 MSVC+CUDA 把
# whisper.cpp 单独构建为 whisper.dll（见 tools/build-whisper-cuda.sh），
# 主程序（MinGW）以动态库方式链接。whisper 的 C API 是纯 C 接口，跨编译器 ABI 兼容。
option(MEETMIND_WHISPER_CUDA_DLL "接入预编译的 CUDA 版 whisper.dll（MinGW 无法直接编译 CUDA）" OFF)
set(MEETMIND_WHISPER_CUDA_DLL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/whisper-cuda-dll"
    CACHE PATH "CUDA 版 whisper.dll 及依赖放置目录")

# Qt6 路径提示（也可通过 -DCMAKE_PREFIX_PATH=... 指定）
set(MEETMIND_QT_ROOT "" CACHE PATH "Qt6 安装根目录，例如 C:/Qt/6.7.1/mingw_64")

# ---------------------------------------------------------------- 编译选项
# 只作用于本项目目标（INTERFACE 目标），避免把严格告警施加到第三方源码上。
add_library(mm_options INTERFACE)
target_compile_features(mm_options INTERFACE cxx_std_17)

if(MSVC)
    target_compile_options(mm_options INTERFACE /W4 /utf-8 /permissive-)
else()
    target_compile_options(mm_options INTERFACE
        -Wall -Wextra -Wpedantic
        -finput-charset=UTF-8 -fexec-charset=UTF-8
        -Wno-unused-parameter)
    if(MEETMIND_WARNINGS_AS_ERRORS)
        target_compile_options(mm_options INTERFACE -Werror)
    endif()
endif()

# Windows 平台定义（目录级，第三方子目录亦需一致的 API 版本声明）：
#   UNICODE/_UNICODE    宽字符 API
#   NOMINMAX            禁用 <windows.h> 的 min/max 宏
#   WIN32_LEAN_AND_MEAN 精简 <windows.h>
#   _WIN32_WINNT/WINVER 目标最低系统为 Windows 10
#                        （whisper.cpp 的 ggml-cpu 需要 Windows 8+ 的线程节流 API 类型）
if(WIN32)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
                            _WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()

find_package(Threads REQUIRED)

# ============================================================================
#  mm_core — 核心库
# ============================================================================
file(GLOB_RECURSE MM_CORE_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/mm/common/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/mm/audio/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/mm/asr/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/mm/diar/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/mm/nlp/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/mm/pipeline/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/mm/live/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/mm/storage/*.cpp")

add_library(mm_core STATIC ${MM_CORE_SOURCES})
add_library(MeetMind::core ALIAS mm_core)

target_include_directories(mm_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(mm_core PUBLIC Threads::Threads PRIVATE mm_options)

target_compile_definitions(mm_core PUBLIC
    MEETMIND_VERSION="${PROJECT_VERSION}"
    MEETMIND_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# ---------------------------------------------------------------- whisper.cpp
if(MEETMIND_WITH_WHISPER)
    set(_whisper_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/whisper.cpp")

    # ---- 路径 A：接入预编译的 CUDA 版 whisper.dll（GPU 加速）----
    # 由 tools/build-whisper-cuda.sh 用 MSVC+CUDA 构建，产物落在 DLL_DIR。
    # 主程序仍用 MinGW 编译；whisper 的 C API 是纯 C 接口，跨编译器 ABI 兼容
    # （Windows x64 上 MinGW-w64 与 MSVC 共用同一套调用约定，含大结构体按引用传参）。
    if(MEETMIND_WHISPER_CUDA_DLL)
        set(_dll "${MEETMIND_WHISPER_CUDA_DLL_DIR}")
        if(NOT EXISTS "${_dll}/whisper.dll")
            message(FATAL_ERROR
                "MEETMIND_WHISPER_CUDA_DLL=ON，但未找到 ${_dll}/whisper.dll\n"
                "请先运行： bash tools/build-whisper-cuda.sh")
        endif()

        add_library(whisper SHARED IMPORTED GLOBAL)
        # whisper.h 会 include ggml.h，因此 ggml/include 也必须可见；
        # 且必须与构建 DLL 时用的是同一份头文件（同一源码树），否则结构体布局会不一致。
        set_target_properties(whisper PROPERTIES
            IMPORTED_LOCATION             "${_dll}/whisper.dll"
            INTERFACE_INCLUDE_DIRECTORIES "${_whisper_dir}/include;${_whisper_dir}/ggml/include"
            # 消费端需要 dllimport 才能生成正确的间接调用/数据引用
            INTERFACE_COMPILE_DEFINITIONS "WHISPER_SHARED=1")
        # 优先用 dlltool 生成的 MinGW 导入库；否则回退 MSVC 的 .lib
        if(EXISTS "${_dll}/libwhisper.a")
            set_target_properties(whisper PROPERTIES IMPORTED_IMPLIB "${_dll}/libwhisper.a")
        elseif(EXISTS "${_dll}/whisper.lib")
            set_target_properties(whisper PROPERTIES IMPORTED_IMPLIB "${_dll}/whisper.lib")
        else()
            message(FATAL_ERROR "未找到 whisper 导入库（libwhisper.a 或 whisper.lib）于 ${_dll}")
        endif()

        target_link_libraries(mm_core PUBLIC whisper)
        target_compile_definitions(mm_core PUBLIC MEETMIND_WITH_WHISPER=1)
        message(STATUS "MeetMind: whisper.cpp 以 CUDA DLL 方式接入 (${_dll})")

    elseif(EXISTS "${_whisper_dir}/CMakeLists.txt")
        set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
        # OpenMP：ggml 的 CPU 并行后端。MinGW 自带 libgomp，开启可提升多线程推理吞吐。
        if(MEETMIND_GGML_OPENMP)
            set(GGML_OPENMP ON CACHE BOOL "" FORCE)
        else()
            set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
        endif()

        # GPU 后端：需要 CUDA 工具链（含 MSVC 宿主编译器）或 Vulkan SDK。
        # 单独开关，便于在具备条件的环境上一键启用。
        if(MEETMIND_GGML_GPU)
            set(GGML_CUDA ON CACHE BOOL "" FORCE)
            set(CMAKE_CUDA_ARCHITECTURES "61;75;86;89" CACHE STRING "" FORCE)
            message(STATUS "MeetMind: 已请求编译 CUDA 后端（需要 nvcc + MSVC 宿主编译器）")
        endif()
        add_subdirectory("${_whisper_dir}" "${CMAKE_BINARY_DIR}/whisper.cpp" EXCLUDE_FROM_ALL)
        target_link_libraries(mm_core PUBLIC whisper)

        # 工具链兼容性：部分 mingw-w64 版本缺少 THREAD_POWER_THROTTLING_* 声明，
        # 而 ggml-cpu 在 _WIN32_WINNT >= 0x0602 时会用到（Windows 11 线程调度优化）。
        # 这里先做编译探测，仅在缺失时以 -include 方式注入兼容声明，
        # 不修改任何第三方源码。
        if(WIN32 AND NOT MSVC AND TARGET ggml-cpu)
            include(CheckCSourceCompiles)
            set(_mm_saved_required "${CMAKE_REQUIRED_FLAGS}")
            set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -D_WIN32_WINNT=0x0A00")
            check_c_source_compiles("
                #include <windows.h>
                int main(void) {
                    THREAD_POWER_THROTTLING_STATE s;
                    s.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
                    s.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
                    s.StateMask = 0;
                    return (int)(s.Version + s.ControlMask + s.StateMask);
                }
            " MM_HAVE_WIN_THREAD_POWER_THROTTLING)
            set(CMAKE_REQUIRED_FLAGS "${_mm_saved_required}")

            if(MM_HAVE_WIN_THREAD_POWER_THROTTLING)
                message(STATUS "MeetMind: 工具链已提供 THREAD_POWER_THROTTLING_*，无需兼容声明")
            else()
                message(STATUS "MeetMind: 工具链缺少 THREAD_POWER_THROTTLING_*，"
                               "为 ggml-cpu 注入兼容声明")
                target_compile_options(ggml-cpu PRIVATE
                    -include "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mm_win_thread_throttling_shim.h")
            endif()
        endif()

        target_compile_definitions(mm_core PUBLIC MEETMIND_WITH_WHISPER=1)
        message(STATUS "MeetMind: whisper.cpp 已启用 (${_whisper_dir})")
    else()
        message(WARNING "MeetMind: 未找到 third_party/whisper.cpp，自动关闭 MEETMIND_WITH_WHISPER。"
                        "将使用 ReplayAsrEngine 作为回退后端。")
        set(MEETMIND_WITH_WHISPER OFF)
    endif()
endif()

# ============================================================================
#  mm_cli — 命令行工具
# ============================================================================
if(MEETMIND_BUILD_CLI)
    file(GLOB_RECURSE MM_CLI_SOURCES CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/src/mm/cli/*.cpp")
    if(MM_CLI_SOURCES)
        add_executable(mm_cli ${MM_CLI_SOURCES})
        target_link_libraries(mm_cli PRIVATE mm_core mm_options)
        if(WIN32)
            # CommandLineToArgvW 来自 shell32：用于按宽字符取回命令行参数，
            # 否则中文路径参数会以本地 ANSI 编码到达，无法还原。
            target_link_libraries(mm_cli PRIVATE shell32)
        endif()
        # 注意：Windows / macOS 文件系统不区分大小写，CLI 不能与 GUI 的 MeetMind.exe
        # 同名，否则两个目标会互相覆盖。
        set_target_properties(mm_cli PROPERTIES OUTPUT_NAME "meetmind-cli")
    else()
        message(STATUS "MeetMind: src/mm/cli 为空，跳过 CLI 目标")
    endif()
endif()

# ============================================================================
#  mm_tests — 测试
# ============================================================================
if(MEETMIND_BUILD_TESTS)
    enable_testing()
    file(GLOB_RECURSE MM_TEST_SOURCES CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
    if(MM_TEST_SOURCES)
        add_executable(mm_tests ${MM_TEST_SOURCES})
        target_link_libraries(mm_tests PRIVATE mm_core mm_options)
        target_include_directories(mm_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        target_compile_definitions(mm_tests PRIVATE
            MEETMIND_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
            MEETMIND_TEST_OUT_DIR="${CMAKE_BINARY_DIR}/test-output")
        add_test(NAME meetmind_unit COMMAND mm_tests --verbose)
    endif()
endif()

# ============================================================================
#  mm_gui — Qt 图形界面
# ============================================================================
if(MEETMIND_BUILD_GUI)
    set(_qt_hints "")
    if(MEETMIND_QT_ROOT)
        list(APPEND _qt_hints "${MEETMIND_QT_ROOT}")
    endif()
    # Multimedia 仅用于「实时转写」的麦克风来源；缺失时该模式自动隐藏（文件模拟仍可用）
    find_package(Qt6 QUIET COMPONENTS Widgets Network Multimedia HINTS ${_qt_hints})
    if(Qt6_FOUND)
        set(CMAKE_AUTOMOC ON)
        set(CMAKE_AUTORCC ON)
        set(CMAKE_AUTOUIC ON)
        file(GLOB_RECURSE MM_GUI_SOURCES CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/src/mm/gui/*.cpp"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/mm/gui/*.h")
        add_executable(mm_gui ${MM_GUI_SOURCES})
        target_link_libraries(mm_gui PRIVATE mm_core mm_options Qt6::Widgets Qt6::Network)
        set_target_properties(mm_gui PROPERTIES
            OUTPUT_NAME "MeetMind"
            WIN32_EXECUTABLE OFF)   # 保留控制台输出，便于查看日志；发布时可置 ON
        if(Qt6Multimedia_FOUND)
            target_link_libraries(mm_gui PRIVATE Qt6::Multimedia)
            target_compile_definitions(mm_gui PRIVATE MM_GUI_HAS_MULTIMEDIA=1)
            message(STATUS "MeetMind: Qt Multimedia 可用 → 实时转写支持麦克风")
        else()
            message(STATUS "MeetMind: 未找到 Qt Multimedia → 实时转写仅支持文件模拟")
        endif()
        message(STATUS "MeetMind: Qt6 ${Qt6_VERSION} 已找到，构建 GUI")
    else()
        message(WARNING "MeetMind: 未找到 Qt6，跳过 GUI。"
                        "可指定 -DMEETMIND_QT_ROOT=<Qt6 mingw_64 目录>")
    endif()
endif()

# ============================================================================
#  GPU 运行时 DLL 随可执行文件落位
# ============================================================================
# whisper.dll / ggml*.dll（CUDA 版）+ CUDA 运行时（cudart/cublas）必须与 exe
# 同目录，否则加载即失败。这里统一从 DLL_DIR 复制，避免手工维护。
if(MEETMIND_WITH_WHISPER AND MEETMIND_WHISPER_CUDA_DLL)
    file(GLOB MM_WHISPER_RUNTIME_DLLS "${MEETMIND_WHISPER_CUDA_DLL_DIR}/*.dll")
    if(NOT MM_WHISPER_RUNTIME_DLLS)
        message(WARNING "MeetMind: ${MEETMIND_WHISPER_CUDA_DLL_DIR} 下没有任何 DLL，GPU 运行时将缺失")
    else()
        foreach(_t mm_cli mm_tests mm_gui)
            if(TARGET ${_t})
                foreach(_d ${MM_WHISPER_RUNTIME_DLLS})
                    add_custom_command(TARGET ${_t} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_d}" "$<TARGET_FILE_DIR:${_t}>")
                endforeach()
            endif()
        endforeach()
        list(LENGTH MM_WHISPER_RUNTIME_DLLS _mm_dll_n)
        message(STATUS "MeetMind: ${_mm_dll_n} 个 GPU 运行时 DLL 将随构建落位")
    endif()
endif()

# ============================================================================
#  构建摘要
# ============================================================================
message(STATUS "")
message(STATUS "====================== MeetMind 构建配置 ======================")
message(STATUS "  版本            : ${PROJECT_VERSION}")
message(STATUS "  构建类型        : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ 标准        : ${CMAKE_CXX_STANDARD}")
message(STATUS "  编译器          : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  CLI             : ${MEETMIND_BUILD_CLI}")
message(STATUS "  GUI             : ${MEETMIND_BUILD_GUI}")
message(STATUS "  Tests           : ${MEETMIND_BUILD_TESTS}")
message(STATUS "  whisper.cpp     : ${MEETMIND_WITH_WHISPER}")
if(MEETMIND_WHISPER_CUDA_DLL)
    message(STATUS "  ASR 后端        : CUDA DLL（GPU 加速）")
elseif(MEETMIND_GGML_GPU)
    message(STATUS "  ASR 后端        : 源码编译 + CUDA")
else()
    message(STATUS "  ASR 后端        : CPU（MinGW 源码编译）")
endif()
message(STATUS "===============================================================")
