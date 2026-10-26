add_library(PwarQualificationDriver SHARED pwarASIO.cpp pwarASIOLog.cpp pwarASIO.def
    ../../protocol/pwar_router.c ../../protocol/latency_manager.c
    ../../deps/asiosdk/common/combase.cpp ../../deps/asiosdk/common/dllentry.cpp
    ../../deps/asiosdk/common/register.cpp)
target_include_directories(PwarQualificationDriver PRIVATE ../../deps/asiosdk/common)
target_compile_definitions(PwarQualificationDriver PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
target_compile_options(PwarQualificationDriver PRIVATE /W4 $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
target_link_libraries(PwarQualificationDriver PRIVATE ws2_32 avrt winmm ole32 advapi32 uuid user32)
set_target_properties(PwarQualificationDriver PROPERTIES OUTPUT_NAME PWARASIO)
