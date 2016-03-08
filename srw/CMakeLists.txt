
include_directories(BEFORE
    SYSTEM
    ${CMAKE_SOURCE_DIR}/foreign/asio/asio/include
)
include_directories(${CMAKE_SOURCE_DIR}/library)
include_directories(${CMAKE_SOURCE_DIR}/cocaine/include)

if (WITH_COCAINE)
    # Find cocaine node service
    include(${CMAKE_SOURCE_DIR}/cmake/Modules/locate_library.cmake)
    locate_library(COCAINE_NODE_SERVICE "cocaine/service/node.hpp" "node.cocaine-plugin.so" "cocaine")
endif() # WITH_COCAINE

find_package(Msgpack REQUIRED)

add_library(elliptics_cocaine STATIC
    srw.cpp
    localnode.cpp
)

set_target_properties(elliptics_cocaine PROPERTIES COMPILE_FLAGS "-fPIC")
target_link_libraries(elliptics_cocaine
    ${ELLIPTICS_LIBRARIES}
    ${MSGPACK_LIBRARIES}
    ${COCAINE_NODE_SERVICE_LIBRARIES}
)
