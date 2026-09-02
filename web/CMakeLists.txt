# PCSX2_WEB targets: the browser runtime module and the unit test module.

set(PCSX2_WEB_INITIAL_MEMORY "805306368" CACHE STRING
	"Initial Wasm linear-memory size for the browser runtime")

add_executable(pcsx2_web_runtime host/pcsx2_web_main.cpp)
target_link_libraries(pcsx2_web_runtime PRIVATE PCSX2_FLAGS PCSX2)
target_include_directories(pcsx2_web_runtime PRIVATE
	"${CMAKE_BINARY_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}/pcsx2")
set_target_properties(pcsx2_web_runtime PROPERTIES
	OUTPUT_NAME "pcsx2-web"
	SUFFIX ".mjs")
target_link_options(pcsx2_web_runtime PRIVATE
	-pthread
	"-Wl,--error-limit=0"
	"-sMODULARIZE=1"
	"-sEXPORT_ES6=1"
	"-sEXPORT_NAME=createPCSX2"
	"-sENVIRONMENT=web,worker"
	"-sMAXIMUM_MEMORY=2147483648"
	"-sINITIAL_MEMORY=${PCSX2_WEB_INITIAL_MEMORY}"
	"-sALLOW_MEMORY_GROWTH=1"
	"-sSTACK_SIZE=8388608"
	"-sDEFAULT_PTHREAD_STACK_SIZE=1048576"
	"-sPTHREAD_POOL_SIZE=(Module['pthreadPoolSize']||8)"
	"-sNO_EXIT_RUNTIME=1"
	"-sFORCE_FILESYSTEM=1"
	"-sEXPORTED_RUNTIME_METHODS=['FS','ccall','cwrap','HEAPU8','HEAP32','HEAPU32','PThread','UTF8ToString','stringToUTF8','lengthBytesUTF8']"
	"-sEXPORTED_FUNCTIONS=['_malloc','_free']"
	"-sWASM_BIGINT=1"
	"-sASSERTIONS=1"
	"--embed-file=${CMAKE_SOURCE_DIR}/bin/resources/fonts@/pcsx2/resources/fonts"
	"--embed-file=${CMAKE_SOURCE_DIR}/bin/resources/GameIndex.yaml@/pcsx2/resources/GameIndex.yaml")

# Unit tests: the low-dependency common tests plus the core patch and swizzle tests,
# reported as JSON on stdout for the browser lane.
add_subdirectory("${CMAKE_SOURCE_DIR}/3rdparty/googletest" "${CMAKE_CURRENT_BINARY_DIR}/googletest" EXCLUDE_FROM_ALL)

add_executable(pcsx2_web_unit_tests
	host/pcsx2_web_unit_main.cpp
	"${CMAKE_SOURCE_DIR}/tests/ctest/common/byteswap_tests.cpp"
	"${CMAKE_SOURCE_DIR}/tests/ctest/common/filesystem_tests.cpp"
	"${CMAKE_SOURCE_DIR}/tests/ctest/common/path_tests.cpp"
	"${CMAKE_SOURCE_DIR}/tests/ctest/common/small_string_tests.cpp"
	"${CMAKE_SOURCE_DIR}/tests/ctest/common/string_util_tests.cpp"
	"${CMAKE_SOURCE_DIR}/tests/ctest/core/patch_tests.cpp"
	"${CMAKE_SOURCE_DIR}/tests/ctest/core/MockMemoryInterface.h"
	"${CMAKE_SOURCE_DIR}/tests/ctest/core/StubHost.cpp"
	"${CMAKE_SOURCE_DIR}/tests/ctest/core/GS/swizzle_test_main.cpp")
target_link_libraries(pcsx2_web_unit_tests PRIVATE PCSX2_FLAGS PCSX2 common gtest)
target_include_directories(pcsx2_web_unit_tests PRIVATE
	"${CMAKE_BINARY_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}"
	"${CMAKE_SOURCE_DIR}/pcsx2"
	"${CMAKE_SOURCE_DIR}/tests/ctest/core")
set_target_properties(pcsx2_web_unit_tests PROPERTIES
	OUTPUT_NAME "pcsx2-web-units"
	SUFFIX ".mjs")
target_link_options(pcsx2_web_unit_tests PRIVATE
	-pthread
	"-Wl,--error-limit=0"
	"-sMODULARIZE=1"
	"-sEXPORT_ES6=1"
	"-sEXPORT_NAME=createPCSX2Units"
	"-sENVIRONMENT=web,worker"
	"-sALLOW_MEMORY_GROWTH=1"
	"-sINITIAL_MEMORY=268435456"
	"-sMAXIMUM_MEMORY=1073741824"
	"-sSTACK_SIZE=8388608"
	"-sDEFAULT_PTHREAD_STACK_SIZE=1048576"
	"-sPTHREAD_POOL_SIZE=4"
	"-sNO_EXIT_RUNTIME=1"
	"-sFORCE_FILESYSTEM=1"
	"-sEXPORTED_RUNTIME_METHODS=['FS','ccall','callMain','PThread']"
	"-sEXPORTED_FUNCTIONS=['_main','_malloc','_free']"
	"-sWASM_BIGINT=1"
	"-sASSERTIONS=1")
