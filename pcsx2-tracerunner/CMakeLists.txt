add_executable(pcsx2-tracerunner)

if (PACKAGE_MODE)
	install(TARGETS pcsx2-tracerunner DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
	install(TARGETS pcsx2-tracerunner DESTINATION ${CMAKE_SOURCE_DIR}/bin)
endif()

target_sources(pcsx2-tracerunner PRIVATE
	Main.cpp
)

target_include_directories(pcsx2-tracerunner PRIVATE
	"${CMAKE_BINARY_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}/pcsx2"
)

target_link_libraries(pcsx2-tracerunner PRIVATE
	PCSX2_FLAGS
	PCSX2
)
