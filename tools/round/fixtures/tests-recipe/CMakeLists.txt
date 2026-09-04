# Fixture for tools/round/check_tests_recipe.sh. NOT part of the server build — nothing
# in the repository adds this directory, and the round recipe only ever sees it when the
# self-test hands it over as a build context.
#
# The recipe under test passes exactly `-DCMAKE_BUILD_TYPE=…` and `-DBUILD_TESTS=True`,
# so an arm cannot be selected with a `-D`. It is selected by a MARKER FILE that the
# self-test drops into its own copy of this directory:
#
#   (no marker)   one passing test          -> `docker build` must exit 0
#   ARM_FAIL      one passing, one failing  -> `docker build` must exit non-zero
#   ARM_NOTESTS   no tests registered       -> `docker build` must exit non-zero
#
# `LANGUAGES NONE` on purpose. The claim being tested is the shell's pipeline semantics
# and ctest's exit code, not a compiler, so the fixture compiles nothing and an arm costs
# seconds instead of the twenty minutes a server build takes.
cmake_minimum_required(VERSION 3.20)
project(tests_recipe_fixture LANGUAGES NONE)

enable_testing()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ARM_NOTESTS")
  message(STATUS "tests-recipe fixture: arm 'notests' — registering no tests at all")
else()
  # `cmake -E true` / `-E false` are used instead of a script so the fixture needs no
  # interpreter and no source file: the exit code is the whole point.
  add_test(NAME recipe_fixture_passes COMMAND "${CMAKE_COMMAND}" -E true)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ARM_FAIL")
    add_test(NAME recipe_fixture_fails COMMAND "${CMAKE_COMMAND}" -E false)
  endif()
endif()
