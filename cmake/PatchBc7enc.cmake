# FetchContent may rerun the patch step on an already prepared checkout.
find_program(patch_git git REQUIRED)
set(patch "${CMAKE_CURRENT_LIST_DIR}/Bc7enc.patch")
execute_process(COMMAND "${patch_git}" apply --ignore-space-change --reverse --check "${patch}"
  RESULT_VARIABLE applied OUTPUT_QUIET ERROR_QUIET)
if(NOT applied EQUAL 0)
  execute_process(COMMAND "${patch_git}" apply --ignore-space-change "${patch}" COMMAND_ERROR_IS_FATAL ANY)
endif()
