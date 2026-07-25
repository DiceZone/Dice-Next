# Build-number preparation and commit helper.
#
# Rule:
#   - The counter stores the last successfully linked build number.
#   - PREPARE generates counter + 1 without changing the counter file.
#   - COMMIT writes the prepared number only after the executable links.
#   - It RESETS to 1 when the major.minor changes (e.g. 3.0.x → 3.1.0).
#   - A patch bump (3.0.0 → 3.0.1) does NOT reset it.
# The counter file stores "<major>.<minor>:<number>".
#
# Args:
#   -DMODE=PREPARE|COMMIT -DVERSION=<x.y.z> -DOUT=<version_build.cpp>
#   -DCOUNTER=<counter file> -DPENDING=<pending file> -P bump_build.cmake

if(NOT DEFINED VERSION)
  set(VERSION "0.0.0")
endif()
if(NOT DEFINED MODE)
  set(MODE "PREPARE")
endif()
if(NOT DEFINED PENDING)
  set(PENDING "${OUT}.pending")
endif()

string(REGEX MATCH "^([0-9]+)\\.([0-9]+)" _ignore "${VERSION}")
set(KEY "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}")

if(MODE STREQUAL "PREPARE")
  set(NUM 1)
  if(EXISTS "${COUNTER}")
    file(READ "${COUNTER}" _c)
    string(STRIP "${_c}" _c)
    if(_c MATCHES "^([0-9]+\\.[0-9]+):([0-9]+)$")
      if("${CMAKE_MATCH_1}" STREQUAL "${KEY}")
        math(EXPR NUM "${CMAKE_MATCH_2} + 1")
      endif()
    endif()
  endif()

  string(TIMESTAMP TS "%Y-%m-%d %H:%M:%S")
  file(WRITE "${OUT}"
    "#include \"common/version.h\"\n\nnamespace dice {\n\nint buildNumber() { return ${NUM}; }\nstd::string buildTime() { return \"${TS}\"; }\n\n}  // namespace dice\n")
  file(WRITE "${PENDING}" "${KEY}:${NUM}\n")
elseif(MODE STREQUAL "COMMIT")
  if(NOT EXISTS "${PENDING}")
    message(FATAL_ERROR "Missing pending build number: ${PENDING}")
  endif()
  file(READ "${PENDING}" _pending)
  string(STRIP "${_pending}" _pending)
  if(NOT _pending MATCHES "^${KEY}:([0-9]+)$")
    message(FATAL_ERROR "Invalid pending build number: ${_pending}")
  endif()
  file(WRITE "${COUNTER}" "${_pending}\n")
  file(REMOVE "${PENDING}")
else()
  message(FATAL_ERROR "Unsupported build-number mode: ${MODE}")
endif()
