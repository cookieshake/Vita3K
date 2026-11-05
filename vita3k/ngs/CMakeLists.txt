add_library(
	ngs
	STATIC
	src/modules/atrac9.cpp
	src/modules/compressor.cpp
	src/modules/delay.cpp
	src/modules/distortion.cpp
	src/modules/envelope.cpp
	src/modules/equalizer.cpp
	src/modules/filter.cpp
	src/modules/generator.cpp
	src/modules/mixer.cpp
	src/modules/output.cpp
	src/modules/pauser.cpp
	src/modules/pitchshift.cpp
	src/modules/player.cpp
	src/modules/reverb.cpp
	src/definitions.cpp
	src/ngs.cpp
	src/rack_release.cpp
	src/rate_resampler.cpp
	src/route.cpp
	src/scheduler.cpp)

target_include_directories(ngs PUBLIC include)
target_link_libraries(ngs PUBLIC codec)
target_link_libraries(ngs PRIVATE util mem kernel cpu ffmpeg)

if(NOT ANDROID)
	add_executable(
		ngs-relocation-tests
		${PROJECT_SOURCE_DIR}/tools/ing165/tests/ngs_fixture.cpp
		src/ngs.cpp
		src/rack_release.cpp
		src/scheduler.cpp
		src/route.cpp
		src/modules/mixer.cpp
		src/modules/output.cpp)
	target_include_directories(ngs-relocation-tests PRIVATE
		${PROJECT_SOURCE_DIR}/tools/ing165/tests/stubs
		${CMAKE_CURRENT_SOURCE_DIR}/include
		${PROJECT_SOURCE_DIR}/vita3k/mem/include
		${PROJECT_SOURCE_DIR}/vita3k/util/include
		${PROJECT_SOURCE_DIR}/vita3k/codec/include)
	add_test(NAME ngs-relocation COMMAND ngs-relocation-tests)
endif()
