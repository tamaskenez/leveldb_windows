cmake_minimum_required(VERSION 2.8)
project(leveldb)

if(NOT WIN32)
	message(FATAL_ERROR "This CMakeLists.txt is tested only for WIN32. For other platforms please use the makefile.")
endif()

include(CheckIncludeFile)
check_include_file(cstdatomic HAVE_CSTDATOMIC)
check_include_file(atomic HAVE_ATOMIC)
if(HAVE_ATOMIC)
	add_definitions(-DLEVELDB_ATOMIC_PRESENT)
endif()
IF(HAVE_CSTDATOMIC)
	add_definitions(-DLEVELDB_CSTDATOMIC_PRESENT)
endif()

set(INSTALL_TESTS 0)
enable_testing()

# C4244 conversion, possible loss of data
# C4267 size_t conversion, possible loss of data
# C4996 deprecated function
# C4355 this' : used in base member initializer list
if (MSVC)
	add_definitions(/wd4267 /wd4996 /wd4355 /wd4244)
endif()

find_package(snappy QUIET)
if(SNAPPY_FOUND)
	include_directories(${SNAPPY_INCLUDE_DIRS})
	add_definitions(-DSNAPPY)
endif()

if(CMAKE_COMPILER_IS_GNUCXX)
	set(MEMCMP_FLAG "-fno-builtin-memcmp")
endif()

#set default (POSIX) files
set(ENV_FILE util/env_posix.cc)
set(PORT_FILE port/port_posix.cc)

if(APPLE)
	if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
        set(PLATFORM OS_MACOSX)
        add_definitions(${MEMCMP_FLAG} -DOS_MACOSX)
	else()
		message(FATAL_ERROR Unknown platform)
	endif()
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
	set(PLATFORM OS_LINUX)
	add_definitions(${MEMCMP_FLAG} -pthread -DOS_LINUX)
	message(FATAL_ERROR PLATFORM_LDFLAGS must be given to linker, fix this)
	set(PLATFORM_LDFLAGS -pthread)
elseif(ANDROID)
	message(FATAL_ERROR)
	set(PLATFORM OS_ANDROID)
	add_definitions(${MEMCMP_FLAG} -D_REENTRANT -DOS_ANDROID -DLEVELDB_PLATFORM_POSIX)
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
	message(FATAL_ERROR "Unknown platform")
endif()

set(PORTABLE_FILES
	db/builder.cc db/c.cc db/db_impl.cc db/db_iter.cc db/dbformat.cc db/filename.cc
	db/log_reader.cc db/log_writer.cc db/memtable.cc db/repair.cc db/table_cache.cc
	db/version_edit.cc db/version_set.cc db/write_batch.cc table/block.cc
	table/block_builder.cc table/filter_block.cc table/format.cc table/iterator.cc
	table/merger.cc table/table.cc table/table_builder.cc table/two_level_iterator.cc
	util/arena.cc util/bloom.cc util/cache.cc util/coding.cc util/comparator.cc
	util/crc32c.cc util/env.cc ${ENV_FILE} util/filter_policy.cc util/hash.cc
	util/histogram.cc util/logging.cc util/options.cc util/status.cc)

include_directories(. include)

set(SOURCES ${PORTABLE_FILES} ${PORT_FILE})
set(MEMENV_SOURCES helpers/memenv/memenv.cc)

add_library(leveldb ${SOURCES})
target_link_libraries(leveldb ${SNAPPY_LIBRARIES})

add_library(testutil_lib util/testutil.cc)

add_library(testharness_lib util/testharness.cc)
target_link_libraries(testharness_lib testutil_lib)

set(TESTS
	db/autocompact_test
	util/arena_test 
	util/bloom_test 
	db/c_test
	util/cache_test 
	util/coding_test 
	db/corruption_test 
	util/crc32c_test 
	db/db_test 
	db/dbformat_test 
	util/env_test
	util/hash_test
	issues/issue178_test
	issues/issue200_test
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

FILE(GLOB LEVELDB_PUBLIC_HEADERS include/leveldb/*.h)

set_target_properties(leveldb PROPERTIES DEBUG_POSTFIX _d)
set_target_properties(leveldb PROPERTIES PUBLIC_HEADER "${LEVELDB_PUBLIC_HEADERS}")

foreach(t ${TESTS})
	get_filename_component(n ${t} NAME)
	list(APPEND TEST_TARGETS ${n})
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
	add_test(${n} ${n})
endforeach()

set(INSTALL_TO_VERSIONED_SUBDIR 0)

if(INSTALL_TO_VERSIONED_SUBDIR)
	file(STRINGS include/leveldb/db.h v1 REGEX "kMajorVersion *= *")
	file(STRINGS include/leveldb/db.h v2 REGEX "kMinorVersion *= *")
	string(REGEX MATCH "[0-9]+" MAJOR_VERSION ${v1})
	string(REGEX MATCH "[0-9]+" MINOR_VERSION ${v2})
	if(MAJOR_VERSION STREQUAL "" OR MINOR_VERSION STREQUAL "")
		message(FATAL_ERROR "Version number parsing failed, please check db.h and parsing code here")
	endif()

	set(SUBDIR leveldb-${MAJOR_VERSION}.${MINOR_VERSION})
	set(CONFIGSUBDIR ${SUBDIR})
	set(INSTALL_PREFIX_FROM_CONFIG_MODULE .)
else()
	set(SUBDIR .)
	set(CONFIGSUBDIR cmake)
	set(INSTALL_PREFIX_FROM_CONFIG_MODULE ..)
endif()

install(TARGETS leveldb
	RUNTIME DESTINATION ${SUBDIR}/bin
	ARCHIVE DESTINATION ${SUBDIR}/lib
	LIBRARY DESTINATION ${SUBDIR}/lib
	PUBLIC_HEADER DESTINATION ${SUBDIR}/include/leveldb
	)
	
if(INSTALL_TESTS)
	install(TARGETS ${TEST_TARGETS} CONFIGURATIONS Release DESTINATION ${SUBDIR}/bin/)
endif()

configure_file(leveldbConfig.cmake.in leveldbConfig.cmake.out @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/leveldbConfig.cmake.out DESTINATION ${CONFIGSUBDIR} RENAME leveldbConfig.cmake)

