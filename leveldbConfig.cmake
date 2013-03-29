set(LEVELDB_INCLUDE_DIRS
	${CMAKE_CURRENT_LIST_DIR}/include
)

set(LEVELDB_LIBRARIES
	optimized ${CMAKE_CURRENT_LIST_DIR}/lib/leveldb.lib
	debug ${CMAKE_CURRENT_LIST_DIR}/lib/leveldb_d.lib
)
