cmake_minimum_required(VERSION 3.15)
project(v8_runtime_native)

# --- Configuration ---
# Define a base directory for all our pre-built MSVC dependencies
set(DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dependencies" CACHE PATH "Path to pre-built MSVC dependencies")

# Define paths for each dependency
set(V8_DIR "${DEPS_DIR}/v8")
set(LIBUV_DIR "${DEPS_DIR}/libuv")
set(OPENSSL_DIR "${DEPS_DIR}/openssl")

# set(CMAKE_CXX_STANDARD 20)
# set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Find Dependencies (MSVC Method) ---

# --- Include Directories ---
include_directories(${V8_DIR}/include)
include_directories(${LIBUV_DIR}/include)
include_directories(${OPENSSL_DIR}/include)

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

# --- Link Libraries (MSVC Method) ---

# Specify the library search paths explicitly
link_directories(${V8_DIR})
link_directories(${LIBUV_DIR})
link_directories(${OPENSSL_DIR})

target_link_libraries(runtime
	# V8 component libraries
	v8_libbase.lib
	v8_libplatform.lib
	
	# V8 allocator libraries
	allocator_shim.lib
	
	# ICU libraries (required by V8)
	icui18n.lib
	icuuc.lib
	
	# Other V8 dependencies
	zlib.lib
	compression_utils_portable.lib

	# Libuv
	libuv.lib

	# OpenSSL
	libssl.lib
	libcrypto.lib

    # Windows system libraries
    ws2_32.lib
    psapi.lib
    userenv.lib
    dbghelp.lib
    Iphlpapi.lib # Required by libuv on Windows
    winmm.lib    # Required by V8
)

# Platform-specific setup
if(WIN32)
	# This definition is standard for targeting modern Windows.
	add_definitions(-D_WIN32_WINNT=0x0601)

    # If you built V8 with pointer compression, you MUST define this for your application too.
    # Check your V8 build configuration (gn args) to be sure.
    add_definitions(-DV8_COMPRESS_POINTERS)

	# Tell windows.h not to define min and max macros, which conflict with the C++ standard library.
    add_definitions(-DNOMINMAX)

	# Add the MSVC-specific flag for C++20
	target_compile_options(runtime PRIVATE /std:c++20 /Zc:__cplusplus)
else()
    # This block remains for Linux/macOS builds
	add_definitions(-D_XOPEN_SOURCE)
	target_link_libraries(runtime pthread)
endif()