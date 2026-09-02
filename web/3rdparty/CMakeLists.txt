# Browser dependency set for PCSX2_WEB.
#
# zlib, libpng, libjpeg and freetype come from the Emscripten ports (built once
# into the emscripten cache).  lz4, zstd, libwebp, plutovg, plutosvg and
# rapidyaml are built from sha256-pinned release tarballs that are downloaded
# into the build tree at configure time; no FetchContent, no git clones.

set(PCSX2_WEB_DEPS_DIR "${CMAKE_BINARY_DIR}/_deps" CACHE PATH "Where pinned dependency tarballs are unpacked")

function(pcsx2_web_tarball name url sha256 outvar)
	set(archive "${PCSX2_WEB_DEPS_DIR}/${name}.tar.gz")
	set(stamp "${PCSX2_WEB_DEPS_DIR}/${name}.extracted")
	if(NOT EXISTS "${stamp}")
		message(STATUS "Fetching ${name}")
		file(DOWNLOAD "${url}" "${archive}" EXPECTED_HASH SHA256=${sha256} STATUS status)
		list(GET status 0 status_code)
		if(NOT status_code EQUAL 0)
			message(FATAL_ERROR "Failed to download ${url}: ${status}")
		endif()
		file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${PCSX2_WEB_DEPS_DIR}")
		file(WRITE "${stamp}" "${sha256}\n")
	endif()
	set(${outvar} "${PCSX2_WEB_DEPS_DIR}/${name}" PARENT_SCOPE)
endfunction()

function(pcsx2_web_port target port)
	add_library(${target} INTERFACE)
	target_compile_options(${target} INTERFACE "--use-port=${port}")
	target_link_options(${target} INTERFACE "--use-port=${port}")
endfunction()

pcsx2_web_port(pcsx2-web-zlib zlib)
add_library(ZLIB::ZLIB ALIAS pcsx2-web-zlib)
pcsx2_web_port(pcsx2-web-png libpng)
add_library(PNG::PNG ALIAS pcsx2-web-png)
pcsx2_web_port(pcsx2-web-jpeg libjpeg)
add_library(JPEG::JPEG ALIAS pcsx2-web-jpeg)
pcsx2_web_port(pcsx2-web-freetype freetype)
add_library(Freetype::Freetype ALIAS pcsx2-web-freetype)

# lz4 1.10.0
pcsx2_web_tarball(lz4-1.10.0
	"https://github.com/lz4/lz4/releases/download/v1.10.0/lz4-1.10.0.tar.gz"
	537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b
	LZ4_SRC)
add_library(pcsx2-web-lz4 STATIC
	"${LZ4_SRC}/lib/lz4.c"
	"${LZ4_SRC}/lib/lz4hc.c"
	"${LZ4_SRC}/lib/lz4frame.c"
	"${LZ4_SRC}/lib/lz4file.c"
	"${LZ4_SRC}/lib/xxhash.c")
target_include_directories(pcsx2-web-lz4 PUBLIC "${LZ4_SRC}/lib")
add_library(LZ4::LZ4 ALIAS pcsx2-web-lz4)

# zstd 1.5.7
pcsx2_web_tarball(zstd-1.5.7
	"https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz"
	eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
	ZSTD_SRC)
file(GLOB ZSTD_SOURCES
	"${ZSTD_SRC}/lib/common/*.c"
	"${ZSTD_SRC}/lib/compress/*.c"
	"${ZSTD_SRC}/lib/decompress/*.c")
add_library(pcsx2-web-zstd STATIC ${ZSTD_SOURCES})
target_include_directories(pcsx2-web-zstd PUBLIC "${ZSTD_SRC}/lib")
target_compile_definitions(pcsx2-web-zstd PRIVATE ZSTD_DISABLE_ASM=1 ZSTD_MULTITHREAD=0)
add_library(Zstd::Zstd ALIAS pcsx2-web-zstd)

# libwebp 1.6.0
pcsx2_web_tarball(libwebp-1.6.0
	"https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz"
	e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564
	WEBP_SRC)
set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_CWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_DWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_GIF2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_IMG2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_VWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPINFO OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_LIBWEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBP_JS OFF CACHE BOOL "" FORCE)
set(WEBP_ENABLE_SIMD OFF CACHE BOOL "" FORCE)
set(WEBP_USE_THREAD ON CACHE BOOL "" FORCE)
add_subdirectory("${WEBP_SRC}" "${CMAKE_CURRENT_BINARY_DIR}/libwebp" EXCLUDE_FROM_ALL)
add_library(WebP::libwebp ALIAS webp)

# plutovg 1.3.2 / plutosvg 0.0.7
pcsx2_web_tarball(plutovg-1.3.2
	"https://github.com/sammycage/plutovg/archive/v1.3.2/plutovg-1.3.2.tar.gz"
	7bd4e79ce18b1d47517e7e91fbb7cf19d4f01942804a519bc7c0bf32b6325dd5
	PLUTOVG_SRC)
pcsx2_web_tarball(plutosvg-0.0.7
	"https://github.com/sammycage/plutosvg/archive/v0.0.7/plutosvg-0.0.7.tar.gz"
	78561b571ac224030cdc450ca2986b4de915c2ba7616004a6d71a379bffd15f3
	PLUTOSVG_SRC)
set(PLUTOVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(PLUTOSVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(PLUTOSVG_ENABLE_FREETYPE OFF CACHE BOOL "" FORCE)
add_subdirectory("${PLUTOVG_SRC}" "${CMAKE_CURRENT_BINARY_DIR}/plutovg" EXCLUDE_FROM_ALL)
add_subdirectory("${PLUTOSVG_SRC}" "${CMAKE_CURRENT_BINARY_DIR}/plutosvg" EXCLUDE_FROM_ALL)

# rapidyaml 0.12.1 (source tarball bundles c4core)
pcsx2_web_tarball(rapidyaml-0.12.1-src
	"https://github.com/biojppm/rapidyaml/releases/download/v0.12.1/rapidyaml-0.12.1-src.tgz"
	e9efcdd17f86287748793cf21d106e461fcad8d103a3e5a23632afe93828660d
	RYML_SRC)
set(RYML_INSTALL OFF CACHE BOOL "" FORCE)
set(RYML_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(RYML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${RYML_SRC}" "${CMAKE_CURRENT_BINARY_DIR}/rapidyaml" EXCLUDE_FROM_ALL)

# libpcap ABI stub: DEV9 compiles against the headers in 3rdparty/include and
# every call fails at runtime, which makes the PCAP adapter report itself as
# uninitialised.
add_library(pcsx2-web-pcap STATIC pcap-stub/pcap_stub.c)
target_include_directories(pcsx2-web-pcap PUBLIC "${CMAKE_SOURCE_DIR}/3rdparty/include")
add_library(PCAP::PCAP ALIAS pcsx2-web-pcap)

# discord-rpc ABI stub: presence calls are no-ops in the browser.
add_library(discord-rpc STATIC discord-rpc-stub/discord_rpc.cpp)
target_include_directories(discord-rpc PUBLIC "${CMAKE_SOURCE_DIR}/3rdparty/discord-rpc/include")
