# Applies PATCH to the git checkout in the current directory unless it is already applied.
# FetchContent runs a PATCH_COMMAND again whenever CMake reconfigures an existing build directory.
execute_process(
	COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace --reverse --check "${PATCH}"
	RESULT_VARIABLE not_applied
	OUTPUT_QUIET ERROR_QUIET)
if(not_applied EQUAL 0)
	return()
endif()
execute_process(
	COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${PATCH}"
	RESULT_VARIABLE failed)
if(NOT failed EQUAL 0)
	message(FATAL_ERROR "could not apply ${PATCH}")
endif()
