
include_directories(BEFORE
    SYSTEM
    ${PROJECT_SOURCE_DIR}/foreign/asio/asio/include
)
include_directories(../library)
include_directories(../cocaine/include)

if (WITH_COCAINE)
    # Find cocaine node service
    include(../cmake/Modules/locate_library.cmake)
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
