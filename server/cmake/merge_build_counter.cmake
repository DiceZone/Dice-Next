# Git merge driver for server/build_counter.txt.
#
# CURRENT (%A) is overwritten with the resolved value. For the same release
# line, the greatest counter wins regardless of merge/rebase direction. When
# release lines differ, CURRENT wins so a newer branch is never replaced by an
# unrelated counter from another major.minor line.

foreach(ARG ANCESTOR CURRENT INCOMING)
  if(NOT DEFINED ${ARG} OR NOT EXISTS "${${ARG}}")
    message(FATAL_ERROR "Missing merge input ${ARG}: ${${ARG}}")
  endif()
endforeach()

function(read_counter PATH PREFIX)
  file(READ "${PATH}" VALUE)
  string(STRIP "${VALUE}" VALUE)
  if(VALUE MATCHES "^([0-9]+\\.[0-9]+):([0-9]+)$")
    set(${PREFIX}_VALID TRUE PARENT_SCOPE)
    set(${PREFIX}_KEY "${CMAKE_MATCH_1}" PARENT_SCOPE)
    set(${PREFIX}_NUMBER "${CMAKE_MATCH_2}" PARENT_SCOPE)
  else()
    set(${PREFIX}_VALID FALSE PARENT_SCOPE)
  endif()
endfunction()

read_counter("${CURRENT}" LOCAL)
read_counter("${INCOMING}" REMOTE)

if(LOCAL_VALID AND REMOTE_VALID)
  if(LOCAL_KEY STREQUAL REMOTE_KEY)
    if(REMOTE_NUMBER GREATER LOCAL_NUMBER)
      set(RESULT_KEY "${REMOTE_KEY}")
      set(RESULT_NUMBER "${REMOTE_NUMBER}")
    else()
      set(RESULT_KEY "${LOCAL_KEY}")
      set(RESULT_NUMBER "${LOCAL_NUMBER}")
    endif()
  else()
    set(RESULT_KEY "${LOCAL_KEY}")
    set(RESULT_NUMBER "${LOCAL_NUMBER}")
  endif()
elseif(LOCAL_VALID)
  set(RESULT_KEY "${LOCAL_KEY}")
  set(RESULT_NUMBER "${LOCAL_NUMBER}")
elseif(REMOTE_VALID)
  set(RESULT_KEY "${REMOTE_KEY}")
  set(RESULT_NUMBER "${REMOTE_NUMBER}")
else()
  message(FATAL_ERROR "Neither side contains a valid Dice!Next build counter")
endif()

file(WRITE "${CURRENT}" "${RESULT_KEY}:${RESULT_NUMBER}\n")
