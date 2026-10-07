cmake_minimum_required(VERSION 3.20)
project(station_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(STATION_WITH_ORTOOLS "Собрать планировщик CP-SAT (нужен OR-Tools)" ON)

find_package(nlohmann_json 3.10 REQUIRED)

add_library(station STATIC
  src/time.cpp
  src/pyfmt.cpp
  src/sha1.cpp
  src/topology.cpp
  src/model.cpp
  src/placement.cpp
  src/forecast.cpp
  src/conflicts.cpp
  src/checker.cpp
  src/alternatives.cpp
  src/index.cpp
  src/planner.cpp
  src/planner_cpsat.cpp
)
target_include_directories(station PUBLIC include)
target_link_libraries(station PUBLIC nlohmann_json::nlohmann_json)
# -ffp-contract=off: без слияния a*b+c в одну инструкцию FMA — арифметика с плавающей точкой
# совпадает с Python до последнего бита (важно для округлений в текстах и индексе).
target_compile_options(station PUBLIC -ffp-contract=off)
target_compile_options(station PRIVATE -Wall -Wextra)

if(STATION_WITH_ORTOOLS)
  find_package(ortools CONFIG)
  if(ortools_FOUND)
    target_link_libraries(station PUBLIC ortools::ortools)
    target_compile_definitions(station PUBLIC STATION_WITH_ORTOOLS)
    message(STATUS "OR-Tools найден: планировщик CP-SAT включён")
  else()
    message(WARNING "OR-Tools не найден: собирается только эвристический планировщик")
  endif()
endif()

add_executable(station_core app/main.cpp)
target_link_libraries(station_core PRIVATE station)

enable_testing()
add_executable(unit_tests tests/unit_tests.cpp)
target_link_libraries(unit_tests PRIVATE station)
add_test(NAME unit_tests COMMAND unit_tests)
