# Runs probe_socket_fixture with libx4vr_probe.so preloaded and checks the probe's log.
# Inputs: FIXTURE, PROBE, DIR.
set(port 42420)
file(REMOVE_RECURSE "${DIR}")
get_filename_component(name "${FIXTURE}" NAME)
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env LD_PRELOAD=${PROBE} X4VR_PROBE_EXE=${name} X4VR_PROBE_PORT=${port} X4VR_PROBE_DIR=${DIR}
          ${FIXTURE} ${port}
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "fixture failed (${result}): ${output}")
endif()
file(GLOB logs "${DIR}/socket-*.log")
list(LENGTH logs count)
if(NOT count EQUAL 1)
  message(FATAL_ERROR "expected one probe log in ${DIR}, found ${count}")
endif()
file(READ "${logs}" log)
foreach(expected
    "watching UDP port ${port}"
    "bind\\(fd [0-9]+, IPv4 port ${port}, UDP\\) -> 0"
    "TRACKING fd [0-9]+ as X4's OpenTrack socket"
    "recvfrom fd=[0-9]+ buffer=0x[0-9a-f]+ length=64 flags=0x0 -> 48"
    "packet x=1.000 y=0.000 z=0.000 yaw=10.000"
    "poll including the OpenTrack socket, timeout_ms=1000 -> 1"
    "recv fd=[0-9]+ .* flags=0x40 DONTWAIT -> -1 errno=Resource temporarily unavailable"
    "backtrace \\(new caller\\)"
    "close: fd [0-9]+ is no longer tracked"
    "FINAL [0-9]+s: read=0 recv=4 recvfrom=1 recvmsg=0 recvmmsg=0 poll=1")
  if(NOT log MATCHES "${expected}")
    message(FATAL_ERROR "probe log lacks /${expected}/:\n${log}")
  endif()
endforeach()
message(STATUS "probe log OK")
