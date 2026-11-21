cmake_minimum_required(VERSION 3.20)

set(FIXTURE_TARGETS
    step_basics
    stl_containers
    memory_watch
    multithread
    crash_autopsy
    hft_orderbook
)

foreach(tgt IN LISTS FIXTURE_TARGETS)
    add_executable(fixture_${tgt} ${tgt}.cpp)
    target_compile_features(fixture_${tgt} PRIVATE cxx_std_20)
    target_compile_options(fixture_${tgt} PRIVATE -g -O0 -fno-omit-frame-pointer)
    set_target_properties(fixture_${tgt} PROPERTIES
        OUTPUT_NAME "${tgt}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fixtures"
    )
endforeach()
