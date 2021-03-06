# Threading wrapper library.
PROJECT(libi18n CXX)

# Find the system threading library.
# NOTE: If using Windows, always assume we're using Windows threads.
# CMake attempts to find pthreads, which causes conflicts if building
# with MSYS2/MinGW-w64.
IF(WIN32)
	SET(CMAKE_USE_WIN32_THREADS_INIT 1)
	SET(Threads_FOUND TRUE)
ELSE(WIN32)
	FIND_PACKAGE(Threads REQUIRED)
ENDIF(WIN32)

# Threading implementation.
SET(librpthreads_SRCS dummy.cpp)
SET(librpthreads_H
	Atomics.h
	Semaphore.hpp
	Mutex.hpp
	pthread_once.h
	)
IF(CMAKE_USE_WIN32_THREADS_INIT)
	SET(HAVE_WIN32_THREADS 1)
	SET(librpthreads_SRCS ${librpthreads_SRCS} pthread_once.c)
ELSEIF(CMAKE_USE_PTHREADS_INIT)
	SET(HAVE_PTHREADS 1)
ELSE()
	MESSAGE(FATAL_ERROR "No threading model is supported on this system.")
ENDIF()

# Write the config.h file.
CONFIGURE_FILE("${CMAKE_CURRENT_SOURCE_DIR}/config.librpthreads.h.in" "${CMAKE_CURRENT_BINARY_DIR}/config.librpthreads.h")

######################
# Build the library. #
######################

# FIXME: May need a dummy file on Linux?
ADD_LIBRARY(rpthreads STATIC
	${librpthreads_SRCS}
	${librpthreads_H}
	)

TARGET_INCLUDE_DIRECTORIES(rpthreads
	PUBLIC	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
		$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
	)

IF(CMAKE_THREAD_LIBS_INIT)
	TARGET_LINK_LIBRARIES(rpthreads PUBLIC ${CMAKE_THREAD_LIBS_INIT})
ENDIF(CMAKE_THREAD_LIBS_INIT)

# Unix: Add -fpic/-fPIC in order to use this static library in plugins.
IF(UNIX AND NOT APPLE)
	SET(CMAKE_C_FLAGS	"${CMAKE_C_FLAGS} -fpic -fPIC")
	SET(CMAKE_CXX_FLAGS	"${CMAKE_CXX_FLAGS} -fpic -fPIC")
ENDIF(UNIX AND NOT APPLE)
