cmake_minimum_required(VERSION 2.8)
project(leveldb)

#find_package(Snappy)
include_directories(${SNAPPY_INCLUDE_DIRS})

if(CMAKE_COMPILER_IS_GNUCXX)
	set(MEMCMP_FLAG "-fno-builtin-memcmp")
endif()

# C4244 conversion, possible loss of data
# C4267 size_t conversion, possible loss of data
# C4996 deprecated function
# C4355 this' : used in base member initializer list
if (MSVC)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /wd4267 /wd4996 /wd4355 /wd4244")
endif()

#set default (POSIX) files
set(ENV_FILE util/env_posix.cc)
set(PORT_FILE port/port_posix.cc)

if(APPLE)
	if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
        set(PLATFORM OS_MACOSX)
        set(COMMON_FLAGS ${MEMCMP_FLAG} -DOS_MACOSX)
	else()
		message(FATAL_ERROR Unknown platform)
	endif()
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
	set(PLATFORM OS_LINUX)
	set(COMMON_FLAGS ${MEMCMP_FLAG} -pthread -DOS_LINUX)
	set(PLATFORM_LDFLAGS -pthread)
elseif(ANDROID)
message(FATAQL_ERROR)
	set(PLATFORM OS_ANDROID)
	set(COMMON_FLAGS ${MEMCMP_FLAG} -D_REENTRANT -DOS_ANDROID -DLEVELDB_PLATFORM_POSIX)
	set(CROSS_COMPILE 1)
elseif(MSVC)
	set(PLATFORM OS_WIN)
	add_definitions(-DOS_WIN -DLEVELDB_PLATFORM_WINDOWS -D_CRT_SECURE_NO_WARNINGS)
	set(PORT_FILE port/port_win.cc)
	set(ENV_FILE
		util/env_win.cc
		util/env_win_detail/win_misc.cc
		util/env_win_detail/win_mmap.cc
		util/env_win_detail/win_thread.cc
		util/env_win_detail/win_time.cc
		util/env_win_detail/winapi.cc
		)
else()
	message(FATAL_ERROR Unknown platform)
endif()

set(PORTABLE_FILES
db/builder.cc db/c.cc db/db_impl.cc db/db_iter.cc db/dbformat.cc db/filename.cc
db/log_reader.cc db/log_writer.cc db/memtable.cc db/repair.cc db/table_cache.cc
db/version_edit.cc db/version_set.cc db/write_batch.cc table/block.cc table/block_builder.cc table/filter_block.cc table/format.cc table/iterator.cc table/merger.cc table/table.cc table/table_builder.cc table/two_level_iterator.cc util/arena.cc util/bloom.cc util/cache.cc util/coding.cc util/comparator.cc util/crc32c.cc util/env.cc ${ENV_FILE} util/filter_policy.cc util/hash.cc util/histogram.cc util/logging.cc util/options.cc util/status.cc)

if(SNAPPY_FOUND)
	set(COMMON_FLAGS ${COMMON_FLAGS} -DSNAPPY)
endif()

set(CMAKE_C_FLAGS ${CMAKE_C_FLAGS} ${PLATFORM_CCFLAGS} ${COMMON_FLAGS})
set(CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS} ${PLATFORM_CXXFLAGS} ${COMMON_FLAGS})

include_directories(. include)

set(SOURCES ${PORTABLE_FILES} ${PORT_FILE})
set(MEMENV_SOURCES helpers/memenv/memenv.cc)

add_library(leveldb ${SOURCES})

add_library(testutil_lib util/testutil.cc)

add_library(testharness_lib util/testharness.cc)
target_link_libraries(testharness_lib testutil_lib)

set(TESTS
	util/arena_test 
	util/bloom_test 
	util/cache_test 
	util/coding_test 
	db/corruption_test 
	util/crc32c_test 
	db/db_test 
	db/dbformat_test 
	util/env_test 
	db/filename_test 
	table/filter_block_test 
	db/log_test 
	helpers/memenv/memenv_test 
	db/skiplist_test 
	table/table_test 
	db/version_edit_test 
	db/version_set_test 
	db/write_batch_test
)

if(NOT MSVC)
	list(APPEND TESTS db/c_test)
endif()

foreach(t ${TESTS})
	get_filename_component(n ${t} NAME)
	if (n STREQUAL memenv_test)
		add_executable(${n}
			${t}
			helpers/memenv/memenv.cc)
		target_link_libraries(${n} testharness_lib leveldb)
	else()
		add_executable(${n}
			${t})
		target_link_libraries(${n} testharness_lib leveldb)
	endif()
endforeach()
