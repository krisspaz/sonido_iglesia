if(NOT DEFINED CSP_PROJECT_ROOT)
    message(FATAL_ERROR "CSP_PROJECT_ROOT was not provided")
endif()

set(lock_file "${CSP_PROJECT_ROOT}/driver/upstream.lock.json")
set(patch_file "${CSP_PROJECT_ROOT}/driver/patches/0001-church-stream-virtual-cable.patch")
set(build_script "${CSP_PROJECT_ROOT}/scripts/build-virtual-driver.ps1")

foreach(required_file IN ITEMS "${lock_file}" "${patch_file}" "${build_script}")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Required virtual-driver source file is missing: ${required_file}")
    endif()
endforeach()

file(READ "${lock_file}" lock_json)
string(JSON expected_hash GET "${lock_json}" patchSha256)
string(JSON upstream_commit GET "${lock_json}" commit)
file(SHA256 "${patch_file}" actual_hash)

string(TOLOWER "${expected_hash}" expected_hash)
string(TOLOWER "${actual_hash}" actual_hash)
if(NOT actual_hash STREQUAL expected_hash)
    message(FATAL_ERROR "Virtual-driver patch hash mismatch: expected ${expected_hash}, got ${actual_hash}")
endif()

string(LENGTH "${upstream_commit}" upstream_commit_length)
if(NOT upstream_commit_length EQUAL 40 OR upstream_commit MATCHES "[^0-9a-f]")
    message(FATAL_ERROR "The pinned SysVAD revision is not a full 40-character commit")
endif()

file(READ "${patch_file}" patch_text)
foreach(required_text IN ITEMS
    "VirtualCableWrite"
    "VirtualCableRead"
    "VirtualCableCapacityBytes = 192000"
    "m_pWfExt->Format.nSamplesPerSec != 48000"
    "KSAUDIO_SPEAKER_STEREO"
)
    string(FIND "${patch_text}" "${required_text}" found_at)
    if(found_at EQUAL -1)
        message(FATAL_ERROR "Virtual-driver patch is missing required implementation text: ${required_text}")
    endif()
endforeach()

file(READ "${build_script}" build_script_text)
foreach(required_text IN ITEMS
    "ChurchStreamVirtual.inf"
    "ChurchStreamVirtual.sys"
    "ChurchStreamVirtual.cat"
    "Church Stream Processor Output"
    "SignMode=Off"
    "package-unsigned"
)
    string(FIND "${build_script_text}" "${required_text}" found_at)
    if(found_at EQUAL -1)
        message(FATAL_ERROR "Virtual-driver build script is missing required safety text: ${required_text}")
    endif()
endforeach()

message(STATUS "Virtual-driver source manifest verified at SysVAD ${upstream_commit}")
