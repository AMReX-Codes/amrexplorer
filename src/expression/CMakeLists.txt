add_library(amrexplorer_expression
    Expression.cpp
)
add_library(amrexplorer::expression ALIAS amrexplorer_expression)

target_include_directories(amrexplorer_expression
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(amrexplorer_expression
    PRIVATE amrexplorer::warnings
)
target_compile_features(amrexplorer_expression PUBLIC cxx_std_20)
