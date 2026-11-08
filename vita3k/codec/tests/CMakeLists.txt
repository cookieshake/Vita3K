set(VITA3K_ROOT "${PROJECT_SOURCE_DIR}")
set(VITA3K_TEST_STUBS "${VITA3K_ROOT}/vita3k/tests/support/stubs")

add_executable(
	aac-output-tests
	aac_output_tests.cpp
	${VITA3K_ROOT}/vita3k/codec/src/aac.cpp
	${VITA3K_ROOT}/vita3k/codec/src/decoder.cpp)
target_include_directories(aac-output-tests PRIVATE
	"${VITA3K_TEST_STUBS}"
	"${VITA3K_ROOT}/vita3k/codec/include"
	"${VITA3K_ROOT}/vita3k/mem/include"
	"${VITA3K_ROOT}/external/ffmpeg/include"
	"${VITA3K_ROOT}/vita3k/util/include")
target_link_libraries(aac-output-tests PRIVATE ffmpeg)
add_test(NAME aac-output COMMAND aac-output-tests)
