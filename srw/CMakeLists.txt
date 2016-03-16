
include_directories(BEFORE
    SYSTEM
    ${CMAKE_SOURCE_DIR}/foreign/asio/asio/include
)
include_directories(${CMAKE_SOURCE_DIR}/library)
include_directories(${CMAKE_SOURCE_DIR}/cocaine/include)

if (WITH_COCAINE)
    #FIXME: make cmake module for finding node service?
    # Find cocaine node service
    include(${CMAKE_SOURCE_DIR}/cmake/Modules/locate_library.cmake)
    locate_library(COCAINE_NODE_SERVICE "cocaine/service/node.hpp" "cocaine-plugin-node")
endif() # WITH_COCAINE

find_package(Msgpack REQUIRED)

add_library(elliptics_srw STATIC
    srw.cpp
    localnode.cpp
)

set_target_properties(elliptics_srw PROPERTIES COMPILE_FLAGS "-fPIC")
target_link_libraries(elliptics_srw
    ${ELLIPTICS_LIBRARIES}
    ${MSGPACK_LIBRARIES}
    ${COCAINE_NODE_SERVICE_LIBRARIES}

    #FIXME: should add it properly
    blackhole
)
