set(VITA3K_ROOT "${PROJECT_SOURCE_DIR}")
set(VITA3K_TEST_STUBS "${VITA3K_ROOT}/vita3k/tests/support/stubs")

add_executable(
	ngs-relocation-tests
	rack_relocation_tests.cpp
	${VITA3K_ROOT}/vita3k/ngs/src/ngs.cpp
	${VITA3K_ROOT}/vita3k/ngs/src/rack_release.cpp
	${VITA3K_ROOT}/vita3k/ngs/src/scheduler.cpp
	${VITA3K_ROOT}/vita3k/ngs/src/route.cpp
	${VITA3K_ROOT}/vita3k/ngs/src/modules/mixer.cpp
	${VITA3K_ROOT}/vita3k/ngs/src/modules/output.cpp)
target_include_directories(ngs-relocation-tests PRIVATE
	"${VITA3K_TEST_STUBS}"
	"${VITA3K_ROOT}/vita3k/ngs/include"
	"${VITA3K_ROOT}/vita3k/mem/include"
	"${VITA3K_ROOT}/vita3k/util/include"
	"${VITA3K_ROOT}/vita3k/codec/include")
add_test(NAME ngs-relocation COMMAND ngs-relocation-tests)
