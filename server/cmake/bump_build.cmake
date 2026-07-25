# Build-number bumper (run at configure and before every build).
#
# Rule (see docs/versioning.md):
#   - The (NNN) build number increments by 1 each compile.
#   - It RESETS to 1 when the major.minor changes (e.g. 3.0.x → 3.1.0).
#   - A patch bump (3.0.0 → 3.0.1) does NOT reset it.
# So the "reset key" is "<major>.<minor>". The counter file stores "KEY:NUM".
#
# Args: -DVERSION=<x.y.z> -DOUT=<version_build.h> -DCOUNTER=<counter file> -P bump_build.cmake

if(NOT DEFINED VERSION)
  set(VERSION "0.0.0")
endif()

string(REGEX MATCH "^([0-9]+)\\.([0-9]+)" _ignore "${VERSION}")
set(KEY "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}")

set(NUM 1)
if(EXISTS "${COUNTER}")
  file(READ "${COUNTER}" _c)
  string(STRIP "${_c}" _c)
  if(_c MATCHES "^([0-9]+\\.[0-9]+):([0-9]+)$")
    if("${CMAKE_MATCH_1}" STREQUAL "${KEY}")
      math(EXPR NUM "${CMAKE_MATCH_2} + 1")   # same major.minor → keep counting
    endif()
    # different major.minor → NUM stays 1 (reset)
  endif()
endif()

file(WRITE "${COUNTER}" "${KEY}:${NUM}\n")
string(TIMESTAMP TS "%Y-%m-%d %H:%M:%S")
file(WRITE "${OUT}"
  "#include \"common/version.h\"\n\nnamespace dice {\n\nint buildNumber() { return ${NUM}; }\nstd::string buildTime() { return \"${TS}\"; }\n\n}  // namespace dice\n")
