cmake_minimum_required(VERSION 3.16)
project(NovaLang LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(LLVM REQUIRED CONFIG)
find_package(BISON 3.0 REQUIRED)
find_package(FLEX REQUIRED)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

bison_target(Parser src/parser.y ${CMAKE_CURRENT_BINARY_DIR}/parser.cpp
             DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/parser.hpp)
flex_target(Lexer src/lexer.l ${CMAKE_CURRENT_BINARY_DIR}/lexer.cpp)
add_flex_bison_dependency(Lexer Parser)

add_executable(novac
  src/main.cpp
  src/ast.cpp
  src/sema.cpp
  src/codegen.cpp
  ${BISON_Parser_OUTPUTS}
  ${FLEX_Lexer_OUTPUTS}
)

target_include_directories(novac PRIVATE src ${CMAKE_CURRENT_BINARY_DIR} ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
target_compile_definitions(novac PRIVATE ${LLVM_DEFINITIONS_LIST})

if(LLVM_LINK_LLVM_DYLIB)
  target_link_libraries(novac PRIVATE LLVM)
else()
  llvm_map_components_to_libnames(LLVM_LIBS core support passes irreader native target)
  target_link_libraries(novac PRIVATE ${LLVM_LIBS})
endif()

enable_testing()
add_test(NAME language_tests
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.py $<TARGET_FILE:novac>
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/cases)
