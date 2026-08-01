# aud_wasm — the Embind module. See M01 for the full link-flag rationale (FILESYSTEM=0, ES6 module
# output, mimalloc, etc).

add_executable(aud_wasm bindings.cpp)

target_link_libraries(aud_wasm PRIVATE aud::core)

target_link_options(aud_wasm PRIVATE
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sENVIRONMENT=web,worker
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=33554432
    -sMAXIMUM_MEMORY=2147483648
    -sSTACK_SIZE=1048576
    -sEXPORTED_RUNTIME_METHODS=['HEAPF32','HEAPU8','HEAPU32']
    -sFILESYSTEM=0
    -sMALLOC=mimalloc
    --bind
)

set_target_properties(aud_wasm PROPERTIES
    OUTPUT_NAME "aud_wasm"
    SUFFIX ".js"
)

if (AUD_ENABLE_PROFILING)
    target_link_options(aud_wasm PRIVATE --profiling-funcs)
endif()
