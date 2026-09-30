# Writes ${BIN_DIR}/generated/fw_build_id.h at BUILD time (R3-a), run by the
# fw_build_id target of CMakeLists.txt with
#   cmake -DSRC_DIR=<project dir> -DBIN_DIR=<build dir> -P fw_build_id.cmake
#
# FW_GIT_REV   git rev-parse --short HEAD, "-dirty" when git status
#              --porcelain -uno lists anything (tracked files only).
# FW_BUILD_ID  first 16 hex digits of the SHA-256 of, byte for byte:
#                "HEAD=" <git rev-parse HEAD, all 40 hex digits>      (R3-b)
#                <stdout of git diff --no-color HEAD>
#                "CMAKE_C_FLAGS=" <CMAKE_C_FLAGS of CMakeCache.txt>
#                " PRESET=" <FW_PRESET of CMakeCache.txt>
#              with no newline added anywhere: file(APPEND) writes "\n" as
#              CR LF on Windows. The id therefore changes with the commit too.
#              tools/gpio_hw_check.py (host_build_id) computes the same over
#              its checkout; tools/run_warm_resets.py reuses that function.
# Either is "unknown" when git fails: never an invented value.
#
# The header is rewritten only when its content changes, so an unchanged tree
# does not force a recompilation.

set(out_dir "${BIN_DIR}/generated")
set(out     "${out_dir}/fw_build_id.h")
set(payload "${out_dir}/fw_build_id.payload")
set(part_head "${out_dir}/fw_build_id.head")
set(part_diff "${out_dir}/fw_build_id.diff")
set(part_tail "${out_dir}/fw_build_id.tail")
file(MAKE_DIRECTORY "${out_dir}")

function(cache_value name result)
    set(value "")
    file(STRINGS "${BIN_DIR}/CMakeCache.txt" lines REGEX "^${name}:[A-Z]+=")
    if(lines)
        list(GET lines 0 line)
        string(REGEX REPLACE "^${name}:[A-Z]+=" "" value "${line}")
    endif()
    set(${result} "${value}" PARENT_SCOPE)
endfunction()

set(rev "unknown")
set(build_id "unknown")

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY "${SRC_DIR}"
    OUTPUT_VARIABLE head
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE head_rc
    ERROR_QUIET
)
execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY "${SRC_DIR}"
    OUTPUT_VARIABLE head_full
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE head_full_rc
    ERROR_QUIET
)
execute_process(
    COMMAND git status --porcelain -uno
    WORKING_DIRECTORY "${SRC_DIR}"
    OUTPUT_VARIABLE status
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE status_rc
    ERROR_QUIET
)
# The diff goes through a file, not a variable, so its bytes reach the hash
# unchanged (no encoding conversion of the console output).
execute_process(
    COMMAND git diff --no-color HEAD
    WORKING_DIRECTORY "${SRC_DIR}"
    OUTPUT_FILE "${part_diff}"
    RESULT_VARIABLE diff_rc
    ERROR_QUIET
)

if((head_rc EQUAL 0) AND head AND (head_full_rc EQUAL 0) AND head_full AND
   (status_rc EQUAL 0) AND (diff_rc EQUAL 0))
    if(status)
        set(rev "${head}-dirty")
    else()
        set(rev "${head}")
    endif()
    cache_value(CMAKE_C_FLAGS flags)
    cache_value(FW_PRESET preset)
    file(WRITE "${part_head}" "HEAD=${head_full}")
    file(WRITE "${part_tail}" "CMAKE_C_FLAGS=${flags} PRESET=${preset}")
    # cmake -E cat copies the three files byte for byte into one.
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E cat "${part_head}" "${part_diff}" "${part_tail}"
        OUTPUT_FILE "${payload}"
        RESULT_VARIABLE cat_rc
    )
    if(cat_rc EQUAL 0)
        file(SHA256 "${payload}" digest)
        string(SUBSTRING "${digest}" 0 16 build_id)
    endif()
endif()
file(REMOVE "${payload}" "${part_head}" "${part_diff}" "${part_tail}")

set(content "/* Generated at build time by cmake/fw_build_id.cmake: do not edit. */
#ifndef FW_BUILD_ID_H
#define FW_BUILD_ID_H
#define FW_GIT_REV  \"${rev}\"
#define FW_BUILD_ID \"${build_id}\"
#endif
")
file(WRITE "${out}.tmp" "${content}")
file(COPY_FILE "${out}.tmp" "${out}" ONLY_IF_DIFFERENT)
file(REMOVE "${out}.tmp")
