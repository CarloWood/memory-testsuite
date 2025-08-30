include(AICxxProject)

add_executable(deque_allocator_test deque_allocator_test.cxx)
target_link_libraries(deque_allocator_test PRIVATE ${AICXX_OBJECTS_LIST})

add_executable(memory_test memory_test.cxx)
if (CW_BUILD_TYPE_IS_DEBUG)
  target_compile_options(memory_test PRIVATE "-O2")
endif()
target_link_libraries(memory_test PRIVATE ${AICXX_OBJECTS_LIST})

add_executable(pool_test pool_test.cxx)
target_link_libraries(pool_test PRIVATE ${AICXX_OBJECTS_LIST})

add_executable(ABA_test ABA_test.cxx)
target_link_libraries(ABA_test PRIVATE ${AICXX_OBJECTS_LIST})
