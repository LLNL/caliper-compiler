find_package(caliper REQUIRED)
find_program(instrumenter caliper_instrumenter)
include_directories(${caliper_INCLUDE_PATH} AFTER)
set(caliper_libs caliper caliper-common caliper-reader)
function(instrument_target target_to_instrument)
  get_target_property(inst_target_source ${target_to_instrument} SOURCES)
  set(inst_list "")
  #add_custom_target(${target_to_instrument}_deps
  #  COMMAND cp -r -n ${CMAKE_CURRENT_SOURCE_DIR}/* ${CMAKE_CURRENT_BINARY_DIR}/
  #)
  foreach(tgt_source_name ${inst_target_source})
    set(tgt_source "${CMAKE_CURRENT_SOURCE_DIR}/${tgt_source_name}")
    set(instrumented_file_name "${CMAKE_CURRENT_BINARY_DIR}/${tgt_source_name}")
    add_custom_target(${tgt_source_name}_deps
      COMMAND cp -r -n ${CMAKE_CURRENT_SOURCE_DIR}/* ${CMAKE_CURRENT_BINARY_DIR}/
    )
    add_custom_command(OUTPUT ${instrumented_file_name}
      COMMAND ${instrumenter} ${tgt_source} .* > ${instrumented_file_name}
      DEPENDS ${tgt_source_name}_deps
      VERBATIM
    )
    list(APPEND inst_list ${instrumented_file_name}) 
  endforeach()
  set_target_properties(${target_to_instrument} PROPERTIES SOURCES "${inst_list}")
  get_target_property(inst_target_source2 ${target_to_instrument} SOURCES)
  target_link_libraries(${target_to_instrument} ${caliper_libs})
  #ADD_DEPENDENCIES(${target_to_instrument} ${target_to_instrument}_deps)
endfunction(instrument_target)

