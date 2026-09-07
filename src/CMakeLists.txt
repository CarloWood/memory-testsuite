include(AICxxProject)

add_executable(deque_allocator_test deque_allocator_test.cxx)
target_link_libraries(deque_allocator_test PRIVATE ${AICXX_OBJECTS_LIST})

add_executable(VectorAllocator_test VectorAllocator_test.cxx)
# VectorAllocator exposes C++23's std::allocation_result through allocate_at_least.
target_compile_features(VectorAllocator_test PRIVATE cxx_std_23)
target_link_libraries(VectorAllocator_test PRIVATE ${AICXX_OBJECTS_LIST})

add_executable(memory_test memory_test.cxx)
if (CW_BUILD_TYPE_IS_DEBUG)
  target_compile_options(memory_test PRIVATE "-O2")
endif()
target_link_libraries(memory_test PRIVATE ${AICXX_OBJECTS_LIST})

add_executable(pool_test pool_test.cxx)
target_link_libraries(pool_test PRIVATE ${AICXX_OBJECTS_LIST})

add_executable(ABA_test ABA_test.cxx)
target_link_libraries(ABA_test PRIVATE ${AICXX_OBJECTS_LIST})

add_executable(MemoryMappedPool_constructor MemoryMappedPool_constructor.cxx)
target_link_libraries(MemoryMappedPool_constructor PRIVATE ${AICXX_OBJECTS_LIST})
