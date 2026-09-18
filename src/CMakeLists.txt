include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(analyze)
add_subdirectory(record)
add_subdirectory(index)
add_subdirectory(system)
add_subdirectory(execution)
add_subdirectory(parser)
add_subdirectory(optimizer)
add_subdirectory(storage)
add_subdirectory(common)
add_subdirectory(replacer)
add_subdirectory(transaction)
add_subdirectory(recovery)
add_subdirectory(test)


target_link_libraries(parser execution pthread)

add_executable(rmdb rmdb.cpp)
target_link_libraries(rmdb parser execution readline pthread planner analyze)

# unit_test
add_executable(unit_test unit_test.cpp)
target_link_libraries(unit_test storage lru_replacer record index gtest_main)  # add gtest
add_test(NAME unit_test COMMAND unit_test)

add_executable(storage_regression ../tests/storage_regression.cpp)
target_link_libraries(storage_regression record index storage lru_replacer gtest_main pthread)
target_compile_definitions(storage_regression PRIVATE RMDB_TEST_IO_FAULTS)
target_link_options(storage_regression PRIVATE -Wl,--wrap=write,--wrap=fdatasync)
add_test(NAME storage_regression COMMAND storage_regression)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME sql_regression COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/test_sql_regressions.py $<TARGET_FILE:rmdb>)
add_test(NAME recovery_regression COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/test_recovery.py $<TARGET_FILE:rmdb>)
add_test(NAME query_regression COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/test_query_regressions.py $<TARGET_FILE:rmdb>)
add_test(NAME server_regression COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/test_server.py $<TARGET_FILE:rmdb>)
set_tests_properties(sql_regression recovery_regression query_regression server_regression PROPERTIES
    RUN_SERIAL TRUE TIMEOUT 180)
