function(itch_add_bench name)
  add_executable(${name} ${name}.cpp)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${name} PRIVATE itch itch_gzip itch_warnings)
endfunction()

itch_add_bench(bench_book)
