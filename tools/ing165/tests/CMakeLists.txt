set(ING165_ROOT "${PROJECT_SOURCE_DIR}")
set(ING165_STUBS "${ING165_ROOT}/tools/ing165/tests/stubs")

add_executable(
	ngs-relocation-tests
	ngs_fixture.cpp
	${ING165_ROOT}/vita3k/ngs/src/ngs.cpp
	${ING165_ROOT}/vita3k/ngs/src/rack_release.cpp
	${ING165_ROOT}/vita3k/ngs/src/scheduler.cpp
	${ING165_ROOT}/vita3k/ngs/src/route.cpp
	${ING165_ROOT}/vita3k/ngs/src/modules/mixer.cpp
	${ING165_ROOT}/vita3k/ngs/src/modules/output.cpp)
target_include_directories(ngs-relocation-tests PRIVATE
	"${ING165_STUBS}"
	"${ING165_ROOT}/vita3k/ngs/include"
	"${ING165_ROOT}/vita3k/mem/include"
	"${ING165_ROOT}/vita3k/util/include"
	"${ING165_ROOT}/vita3k/codec/include")
add_test(NAME ngs-relocation COMMAND ngs-relocation-tests)

add_executable(
	aac-output-tests
	aac_fixture.cpp
	${ING165_ROOT}/vita3k/codec/src/aac.cpp
	${ING165_ROOT}/vita3k/codec/src/decoder.cpp)
target_include_directories(aac-output-tests PRIVATE
	"${ING165_STUBS}"
	"${ING165_ROOT}/vita3k/codec/include"
	"${ING165_ROOT}/vita3k/mem/include"
	"${ING165_ROOT}/external/ffmpeg/include"
	"${ING165_ROOT}/vita3k/util/include")
target_link_libraries(aac-output-tests PRIVATE ffmpeg)
add_test(NAME aac-output COMMAND aac-output-tests)
