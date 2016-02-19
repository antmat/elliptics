add_library(elliptics-extensions MODULE module service storage)

INCLUDE_DIRECTORIES(BEFORE
    SYSTEM
    ${PROJECT_SOURCE_DIR}/foreign/asio/asio/include
)
INCLUDE_DIRECTORIES(../include)
TARGET_LINK_LIBRARIES(elliptics-extensions ${COCAINE_LIBRARIES} elliptics_client elliptics_cpp)

SET_TARGET_PROPERTIES(elliptics-extensions PROPERTIES
        PREFIX ""
        SUFFIX ".cocaine-plugin.so"
)

INSTALL(TARGETS elliptics-extensions
        LIBRARY DESTINATION lib${LIB_SUFFIX}/cocaine
        COMPONENT runtime)
INSTALL(
        DIRECTORY
        ../include/
        DESTINATION include
        COMPONENT development)
