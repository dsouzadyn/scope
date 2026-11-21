cmake_minimum_required(VERSION 3.20)
project(scope_debugger LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Homebrew prefixes on Apple Silicon / Intel macOS
set(LLVM_PREFIX "/opt/homebrew/opt/llvm" CACHE PATH "Homebrew LLVM prefix")
set(GLFW_PREFIX "/opt/homebrew/opt/glfw" CACHE PATH "Homebrew GLFW prefix")
set(FREETYPE_PREFIX "/opt/homebrew/opt/freetype" CACHE PATH "Homebrew FreeType prefix")
set(TREESITTER_PREFIX "/opt/homebrew/opt/tree-sitter" CACHE PATH "Homebrew tree-sitter prefix")

# ------------------------------------------------------------------------------
# 1) Third-party: Dear ImGui (docking + FreeType + GLFW + Metal)
# ------------------------------------------------------------------------------
set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/imgui")
add_library(imgui_lib STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/misc/freetype/imgui_freetype.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_metal.mm
)
target_include_directories(imgui_lib PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${GLFW_PREFIX}/include
    ${FREETYPE_PREFIX}/include/freetype2
)
target_compile_definitions(imgui_lib PUBLIC IMGUI_ENABLE_FREETYPE)
set_source_files_properties(${IMGUI_DIR}/backends/imgui_impl_metal.mm PROPERTIES
    COMPILE_OPTIONS "-fobjc-arc"
)

# ------------------------------------------------------------------------------
# 2) Third-party: tree-sitter-cpp grammar
# ------------------------------------------------------------------------------
set(TS_CPP_DIR "${CMAKE_SOURCE_DIR}/third_party/tree-sitter-cpp")
add_library(ts_cpp STATIC
    ${TS_CPP_DIR}/src/parser.c
    ${TS_CPP_DIR}/src/scanner.c
)
target_include_directories(ts_cpp PRIVATE ${TS_CPP_DIR}/src)
target_compile_options(ts_cpp PRIVATE -O1 -Wno-everything)

# ------------------------------------------------------------------------------
# 3) Core Debugger Engine (liblldb + Tree-sitter)
# ------------------------------------------------------------------------------
add_library(scope_engine STATIC
    src/engine/TypeFormat.cpp
    src/engine/SyntaxHighlighter.cpp
    src/engine/DebuggerEngine.cpp
)
target_include_directories(scope_engine PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${LLVM_PREFIX}/include
    ${TREESITTER_PREFIX}/include
)
target_link_directories(scope_engine PUBLIC
    ${LLVM_PREFIX}/lib
    ${TREESITTER_PREFIX}/lib
)
target_link_libraries(scope_engine PUBLIC
    lldb
    tree-sitter
    ts_cpp
)
set_target_properties(scope_engine PROPERTIES
    BUILD_RPATH "${LLVM_PREFIX}/lib;${TREESITTER_PREFIX}/lib"
)

# ------------------------------------------------------------------------------
# 4) SCOPE Desktop Application (GLFW + Metal + Dear ImGui)
# ------------------------------------------------------------------------------
add_executable(scope
    src/main.mm
    src/app/App.cpp
    src/ui/Theme.cpp
    src/ui/Widgets.cpp
    src/ui/TransportAndTimeline.cpp
    src/ui/CodeCanvas.cpp
    src/ui/InspectorBay.cpp
    src/ui/MemoryBay.cpp
    src/ui/StackAndRegisters.cpp
    src/ui/ProbesAndConsole.cpp
)
set_source_files_properties(src/main.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_link_directories(scope PRIVATE
    ${GLFW_PREFIX}/lib
    ${FREETYPE_PREFIX}/lib
    ${LLVM_PREFIX}/lib
    ${TREESITTER_PREFIX}/lib
)
target_link_libraries(scope PRIVATE
    scope_engine
    imgui_lib
    glfw
    freetype
    "-framework Metal"
    "-framework QuartzCore"
    "-framework Cocoa"
    "-framework IOKit"
    "-framework CoreVideo"
)
set_target_properties(scope PROPERTIES
    BUILD_RPATH "${LLVM_PREFIX}/lib;${GLFW_PREFIX}/lib;${FREETYPE_PREFIX}/lib;${TREESITTER_PREFIX}/lib"
)

# ------------------------------------------------------------------------------
# 5) C++ Test Fixtures & Engine Integration Tests
# ------------------------------------------------------------------------------
add_subdirectory(fixtures)

add_executable(scope_engine_test tests/engine_test.cpp)
target_link_libraries(scope_engine_test PRIVATE scope_engine)
set_target_properties(scope_engine_test PROPERTIES
    BUILD_RPATH "${LLVM_PREFIX}/lib;${TREESITTER_PREFIX}/lib"
)
