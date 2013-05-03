project( fc_malloc )
cmake_minimum_required( VERSION 2.8.8 )

IF( WIN32 )
	ADD_DEFINITIONS( -DBOOST_CONTEXT_NO_LIB )
	ADD_DEFINITIONS( -D_SCL_SECURE_NO_WARNINGS )
	ADD_DEFINITIONS( -D_WIN32_WINNT=0x0501 )
	ADD_DEFINITIONS( -D_CRT_SECURE_NO_WARNINGS )
ELSE(WIN32)
   SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++0x -Wall -Wno-unused-local-typedefs")
ENDIF(WIN32)


#add_executable( m3 malloc3.cpp )
add_executable( fheap bench.cpp )
target_link_libraries( fheap jemalloc )
