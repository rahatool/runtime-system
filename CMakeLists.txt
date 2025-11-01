cmake_minimum_required(VERSION 3.10)
project(v8_runtime_native)

# --- Configuration ---
set(V8_DIR $ENV{V8_DIR} CACHE PATH "Path to the V8 installation directory")
set(V8_DIR "/mingw64")
if(NOT V8_DIR)
	message(FATAL_ERROR "Please set the V8_DIR environment variable.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Find Dependencies ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(UV REQUIRED libuv)
include_directories(${UV_INCLUDE_DIRS})
link_directories(${UV_LIBRARY_DIRS})
find_package(OpenSSL REQUIRED)

# --- Include Directories ---
include_directories(${V8_DIR}/include)
include_directories(${UV_INCLUDE_DIR})
include_directories(${OPENSSL_INCLUDE_DIR})

# --- Source Files ---
add_executable(runtime 
	main.cpp 
	fiber.cpp
	handles.cpp
	primitives_fs.cpp
	primitives_tcp.cpp
	primitives_udp.cpp
	primitives_dns.cpp
	primitives_tls.cpp
	primitives_encoding.cpp
)

# --- Link Libraries ---
target_link_libraries(runtime
	# V8
	v8 v8_libbase v8_libplatform
	# Dependencies
	${UV_LIBRARIES}
	OpenSSL::SSL
	OpenSSL::Crypto
)

# Platform-specific setup
if(WIN32)
	add_definitions(-D_WIN32_WINNT=0x0601)
    # Match MSYS2 V8 build (pointer compression + 31-bit smis on 64-bit + sandbox)
    add_definitions(-DV8_COMPRESS_POINTERS -DV8_31BIT_SMIS_ON_64BIT_ARCH -DV8_ENABLE_SANDBOX)
	target_link_libraries(runtime ws2_32 psapi userenv dbghelp)
else()
	add_definitions(-D_XOPEN_SOURCE)
	target_link_libraries(runtime pthread)
endif()

