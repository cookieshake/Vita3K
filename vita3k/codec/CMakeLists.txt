add_library(
    codec
    STATIC
    include/codec/state.h
    include/codec/types.h
    src/atrac9.cpp
    src/decoder.cpp
    src/aac.cpp
    src/h264.cpp
    src/mjpeg.cpp
    src/mp3.cpp
    src/pcm.cpp
    src/player.cpp
)

target_include_directories(codec PUBLIC include)
target_link_libraries(codec PRIVATE ffmpeg libatrac9 util) 

if(NOT ANDROID)
	add_executable(aac-output-tests ${PROJECT_SOURCE_DIR}/tools/ing165/tests/aac_fixture.cpp)
	target_include_directories(aac-output-tests PRIVATE ${PROJECT_SOURCE_DIR}/external/ffmpeg/include)
	target_link_libraries(aac-output-tests PRIVATE codec ffmpeg)
	add_test(NAME aac-output COMMAND aac-output-tests)
endif()
