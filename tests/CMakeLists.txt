add_library(itch_test_harness STATIC harness.cpp)
target_include_directories(itch_test_harness PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(itch_test_harness PUBLIC itch PRIVATE itch_warnings)

function(itch_add_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE itch_test_harness itch_warnings)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 300)
endfunction()

itch_add_test(test_core)
itch_add_test(test_wire)
itch_add_test(test_framing)
itch_add_test(test_parser)

# Only this suite needs zlib.
itch_add_test(test_gzip)
target_link_libraries(test_gzip PRIVATE ZLIB::ZLIB)
itch_add_test(test_map_book)
itch_add_test(test_pool)
itch_add_test(test_ladder)
