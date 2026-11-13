# Tests support
find_package(GTest REQUIRED)

add_executable(dosbox_tests
    ansi_code_markup_tests.cpp
    sector_media_tests.cpp
    batch_file_tests.cpp
    bit_view_tests.cpp
    bitops_tests.cpp
    clap_plugin_tests.cpp
    cmd_move_tests.cpp
    config_tests.cpp
    dos_files_tests.cpp
    dos_memory_struct_tests.cpp
    dosbox_pause_fsm_tests.cpp
    dosbox_restart_tests.cpp
    dosbox_test_fixture.h
    drives_tests.cpp
    floppy_track_data_tests.cpp
    fraction_tests.cpp
    fs_utils_tests.cpp
    i8042_wide_read_tests.cpp
    image_decoder_tests.cpp
    int10_modes_tests.cpp
    math_utils_tests.cpp
    messages_adjust_tests.cpp
    mixer_tests.cpp
    mount_tests.cpp
    port_containers_tests.cpp
    program_mixer_tests.cpp
    rect_tests.cpp
    rgb_tests.cpp
    ring_buffer_tests.cpp
    rwqueue_tests.cpp
    shader_pragma_parser_tests.cpp
    shell_cmds_tests.cpp
    shell_redirection_tests.cpp
    string_utils_tests.cpp
    # stubs.cpp
    support_tests.cpp
    unicode_tests.cpp
    window_geometry_tests.cpp
    multi_prefix_tests.cpp
)

# Disable some warnings for deliberately flawed test cases
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(dosbox_tests PRIVATE
        "-Wno-effc++"
    )
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(dosbox_tests PRIVATE
            "-Wno-gnu-zero-variadic-macro-arguments"
        )
    else()
        target_compile_options(dosbox_tests PRIVATE
            "-Wno-format-overflow"
        )
    endif()
endif()

target_link_libraries(dosbox_tests PRIVATE
    clap
    GTest::gmock_main
    dosboxcommon
    SDL3::Headers
)

foreach(plugin_target IN ITEMS clap_test_plugin clap_test_incompatible_plugin)
    add_library(${plugin_target} MODULE clap_test_plugin.cpp)
    target_link_libraries(${plugin_target} PRIVATE clap)
    set_target_properties(${plugin_target} PROPERTIES PREFIX "" SUFFIX ".clap")
endforeach()

target_compile_definitions(clap_test_incompatible_plugin PRIVATE
    CLAP_TEST_INCOMPATIBLE_ENTRY
)

target_compile_definitions(dosbox_tests PRIVATE
    CLAP_TEST_PLUGIN_PATH="$<TARGET_FILE:clap_test_plugin>"
    CLAP_TEST_INCOMPATIBLE_PLUGIN_PATH="$<TARGET_FILE:clap_test_incompatible_plugin>"
)
add_dependencies(dosbox_tests clap_test_plugin clap_test_incompatible_plugin)

include(GoogleTest)

# `DISCOVERY_MODE PRE_TEST` is needed for local builds on macOS for test
# discovery to work, otherwise the build will just fail with an error.
#
# Probably related to:
# https://gitlab.kitware.com/cmake/cmake/-/issues/21845
#
gtest_discover_tests(
  dosbox_tests WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  DISCOVERY_MODE PRE_TEST
  DISCOVERY_TIMEOUT 60
)

# These probes run the native emulator headlessly with synthetic floppy images.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    find_program(NASM_EXECUTABLE NAMES nasm)
    if(Python3_Interpreter_FOUND AND NASM_EXECUTABLE)
        function(add_floppy_guest_test name script)
            add_test(NAME Guest.${name}
                COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/${script}"
                        --dosbox "$<TARGET_FILE:dosbox>" --nasm "${NASM_EXECUTABLE}"
                        ${ARGN})
            set_tests_properties(Guest.${name} PROPERTIES LABELS floppy TIMEOUT 300)
        endfunction()

        add_floppy_guest_test(FloppyFormatRate run_floppy_format_rate_probe.py)
        add_floppy_guest_test(FloppyBiosMotor run_floppy_bios_motor_probe.py)
        add_floppy_guest_test(FloppySectorContract run_sector_contract_probe.py)
        add_floppy_guest_test(FloppyFatCache run_fat_floppy_cache_probe.py)
        add_floppy_guest_test(FloppyTiming run_floppy_timing_probe.py)
        add_floppy_guest_test(FloppyReporting run_floppy_reporting_probe.py)
        add_floppy_guest_test(FloppyDiskChange run_floppy_disk_change_probe.py)
        add_floppy_guest_test(FloppyBiosRead run_bios_floppy_read_probe.py)
        add_floppy_guest_test(FloppyBiosWrite run_bios_floppy_read_probe.py --scp-write)
        add_floppy_guest_test(FloppyBiosSectorBuffer run_bios_sector_buffer_probe.py)
        add_floppy_guest_test(FloppySizedOverlayWrite run_bios_sector_buffer_probe.py --overlay-write)
        add_floppy_guest_test(FloppyBiosVerify run_bios_floppy_read_probe.py --verify)
        add_floppy_guest_test(FloppyBiosCrossTrack run_bios_floppy_read_probe.py --cross-track)
        add_floppy_guest_test(FloppyIrqReset run_bios_floppy_read_probe.py --irq-reset)
        add_floppy_guest_test(FloppyIrqInit run_floppy_irq_init_probe.py)
        add_floppy_guest_test(FloppyBiosResult run_floppy_bios_result_probe.py)
        add_floppy_guest_test(FloppyZeroSector run_floppy_zero_sector_probe.py)
        add_floppy_guest_test(FloppyResetPosition run_floppy_reset_position_probe.py)
        add_floppy_guest_test(FloppySeekStatus run_floppy_seek_status_probe.py)
        add_floppy_guest_test(FloppyOverlapWrite run_floppy_overlap_write_probe.py)
        add_floppy_guest_test(FloppyIdOnlyWrite run_floppy_id_only_write_probe.py)
        add_floppy_guest_test(FloppyIdOnlyTiming run_floppy_id_only_timing_probe.py)
        add_floppy_guest_test(FloppyDuplicateIndexWrite run_floppy_duplicate_index_probe.py)
        add_floppy_guest_test(FloppyTerminalCountResult run_floppy_tc_result_probe.py)
        add_floppy_guest_test(FloppyFormatIndex run_floppy_format_index_probe.py)
        add_floppy_guest_test(FloppyBlankReadTrack run_floppy_blank_read_track_probe.py)
        add_floppy_guest_test(FloppyEmptyMfmRpm run_floppy_empty_mfm_probe.py)
        add_floppy_guest_test(FloppyFieldTiming run_floppy_transfer_edges_probe.py --case timing)
        add_floppy_guest_test(FloppyResetPhase run_floppy_transfer_edges_probe.py --case reset-phase)
        add_floppy_guest_test(FloppyWrongCylinder run_floppy_transfer_edges_probe.py --case wrong-cylinder)
        add_floppy_guest_test(FloppyMissingIdSize run_floppy_transfer_edges_probe.py --case missing-size)
        add_floppy_guest_test(FloppyDataLength run_floppy_transfer_edges_probe.py --case dtl)
        add_floppy_guest_test(FloppyOverlayCompact run_floppy_overlay_compact_probe.py)
        add_floppy_guest_test(FloppyPcjrPio run_pcjr_floppy_pio_probe.py)
        add_floppy_guest_test(FloppyPioUnderrunTiming run_floppy_pio_underrun_timing_probe.py)
        add_floppy_guest_test(FloppyPioTrackTiming run_floppy_pio_track_timing_probe.py)
        add_floppy_guest_test(FloppyTrackEot run_floppy_track_eot_probe.py)
        add_floppy_guest_test(FloppyPcjrWatchdog run_pcjr_floppy_pio_probe.py --watchdog)
        add_floppy_guest_test(FloppyReadTrack run_bios_floppy_read_probe.py --read-track)
        add_floppy_guest_test(FloppyLongReadTrack run_long_read_track_probe.py)
        add_floppy_guest_test(FloppyReadTrackSizeCrc run_floppy_read_track_size_probe.py --case crc)
        add_floppy_guest_test(FloppyReadTrackTiming run_floppy_read_track_size_probe.py --case timing)
        add_floppy_guest_test(FloppyIdHead run_floppy_id_search_probe.py --case head)
        add_floppy_guest_test(FloppyIdCompletion run_floppy_id_completion_probe.py)
        add_floppy_guest_test(FloppyIdCrcTiming run_floppy_id_crc_timing_probe.py)
        add_floppy_guest_test(FloppyIndexSearch run_floppy_id_search_probe.py --case index)
        add_floppy_guest_test(FloppyBlankTrack run_floppy_id_search_probe.py --case blank)
        add_floppy_guest_test(FloppyPcjrSectorZero run_pcjr_sector_zero_probe.py)
        add_floppy_guest_test(FloppyFormattedSize run_formatted_floppy_probe.py --case size)
        add_floppy_guest_test(FloppyFormattedCrc run_formatted_floppy_probe.py --case crc)
        add_floppy_guest_test(FloppyFormattedTiming run_formatted_floppy_probe.py --case timing)
        add_floppy_guest_test(FloppyRawController run_raw_floppy_read_probe.py --write --format)
        add_floppy_guest_test(FloppyRawBiosFormat run_raw_floppy_read_probe.py --bios-format)
        add_floppy_guest_test(FloppyScpAttachment run_scp_attachment_probe.py)
        # This probe remounts full flux captures for each interruption scenario.
        set_tests_properties(Guest.FloppyScpAttachment PROPERTIES TIMEOUT 600)
        add_floppy_guest_test(FloppyScpFormatSwitch run_scp_attachment_probe.py --format-switch)
        add_floppy_guest_test(FloppyScpReadRevolutions run_scp_attachment_probe.py --read-revolutions)
        add_floppy_guest_test(FloppyScpBiosFormat run_scp_attachment_probe.py --bios-format)
        add_floppy_guest_test(FloppyRawBootDma run_hxc_mfm_probe.py --boot-dma-raw)
        add_floppy_guest_test(FloppyMfmAttachment run_hxc_mfm_probe.py)
        add_floppy_guest_test(Floppy86fAttachment run_86f_attachment_probe.py)
        add_floppy_guest_test(FloppyTransCopyAttachment run_transcopy_attachment_probe.py)
        add_floppy_guest_test(FloppyTransCopyLayout run_transcopy_layout_probe.py)
        add_floppy_guest_test(FloppyTransCopyFields run_transcopy_layout_probe.py --fields)
        add_floppy_guest_test(FloppyBiosSectorIds run_transcopy_layout_probe.py --sector-ids)
        add_floppy_guest_test(FloppyBiosLegacyDpt run_bios_floppy_dpt_probe.py)
        add_floppy_guest_test(FloppyWriteSafety run_floppy_write_safety_probe.py)
    endif()
endif()
