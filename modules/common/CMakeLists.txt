# --- db_common: the library every other module links ------------------------
set(DB_COMMON_SOURCES
    src/error.cpp
    src/value.cpp
    src/schema.cpp
    src/tokenizer.cpp
    src/condition_parser.cpp
    src/condition.cpp
    src/evaluator.cpp
    src/plan.cpp
    src/ra_parser.cpp
    src/plan_json.cpp
    src/operators.cpp
)
set(DB_COMMON_HEADERS
    include/db/common/error.h
    include/db/common/limits.h
    include/db/common/value.h
    include/db/common/schema.h
    include/db/common/token.h
    include/db/common/tokenizer.h
    include/db/common/condition.h
    include/db/common/evaluator.h
    include/db/common/plan.h
    include/db/common/ra_parser.h
    include/db/common/plan_json.h
    include/db/common/tuple_source.h
    include/db/common/operators.h
)
add_library(db_common STATIC ${DB_COMMON_SOURCES})
target_include_directories(db_common PUBLIC include)
# plan_json.h exposes nlohmann::ordered_json, so users of db_common need it too.
target_link_libraries(db_common PUBLIC nlohmann_json)

# Every public header must compile on its own (no hidden include-order
# dependencies), so each module can include just the piece it needs.
# The generated file mirrors the header's path, so two headers can never map
# to the same file on a case-insensitive filesystem.
set(HEADER_CHECK_SOURCES)
foreach(header ${DB_COMMON_HEADERS})
    set(tu ${CMAKE_CURRENT_BINARY_DIR}/header_check/${header}.cpp)
    file(GENERATE OUTPUT ${tu} CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/${header}\"\n")
    list(APPEND HEADER_CHECK_SOURCES ${tu})
endforeach()
add_library(db_common_header_check OBJECT ${HEADER_CHECK_SOURCES})
target_link_libraries(db_common_header_check PRIVATE db_common)

# --- Bonus 1 CLI: relation-text loader, printers, interactive main ----------
add_library(ra_cli_support STATIC
    cli/relation_lexer.cpp
    cli/parser_context.cpp
    cli/relation_parser.cpp
    cli/relation_loader.cpp
    cli/program.cpp
    cli/vector_source.cpp
    cli/printers.cpp
)
target_include_directories(ra_cli_support PUBLIC cli)
target_link_libraries(ra_cli_support PUBLIC db_common)

add_executable(ra_cli cli/main.cpp)
target_link_libraries(ra_cli PRIVATE ra_cli_support)

# --- Tests ------------------------------------------------------------------
add_executable(common_tests
    tests/test_main.cpp
    tests/bonus1_adapter.cpp
    tests/bonus1_cases.cpp
    tests/tokenizer_tests.cpp
    tests/core_types_tests.cpp
    tests/lexer_tests.cpp
    tests/condition_tests.cpp
    tests/evaluator_tests.cpp
    tests/plan_tests.cpp
    tests/ra_parser_tests.cpp
    tests/plan_json_tests.cpp
    tests/plan_json_physical_tests.cpp
    tests/operator_tests.cpp
    tests/relation_loader_tests.cpp
)
target_link_libraries(common_tests PRIVATE ra_cli_support doctest)
# db::toString(Condition) returns std::string; doctest then needs a second
# toString to turn it into its own String in assertion messages.
target_compile_definitions(common_tests PRIVATE DOCTEST_CONFIG_DOUBLE_STRINGIFY)

add_test(NAME common_tests COMMAND common_tests)
# A tokenizer hang (I-1) must show up as a failure, not a stuck build.
set_tests_properties(common_tests PROPERTIES TIMEOUT 60)

list(TRANSFORM DB_COMMON_SOURCES PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/ OUTPUT_VARIABLE LIBRARY_FILES)
list(TRANSFORM DB_COMMON_HEADERS PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/ OUTPUT_VARIABLE LIBRARY_HEADER_FILES)
# The glob also covers private headers in src/ that are not public headers.
file(GLOB_RECURSE LIBRARY_TREE_FILES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_test(NAME common_library_has_no_io_or_regex
    COMMAND ${CMAKE_COMMAND} "-DFILES=${LIBRARY_FILES};${LIBRARY_HEADER_FILES};${LIBRARY_TREE_FILES}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_forbidden.cmake)
