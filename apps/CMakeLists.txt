function(itch_add_app name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE itch itch_gzip itch_warnings)
endfunction()

itch_add_app(itch_stats)
itch_add_app(itch_histogram)
itch_add_app(itch_replay)
