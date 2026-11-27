cmake_minimum_required(VERSION 3.31)

if(NOT DEFINED BUILD_DIR OR BUILD_DIR STREQUAL "")
  message(FATAL_ERROR "BUILD_DIR is required")
endif()

set(source_dir "${CMAKE_CURRENT_LIST_DIR}/../..")
set(template_path "${source_dir}/cmake/install_config.cmake.in")
file(REAL_PATH "${BUILD_DIR}" resolved_build_dir)
set(test_root "${resolved_build_dir}/install-config-test")
file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${test_root}")

# run_install_script(<case_dir> [EXPECT_FAILURE <fragment>] [EXPECT_OUTPUT <fragment>]
#                    [CONFIG_DIR <dir>] [USER_MODELS_DIR <dir>] [DESTDIR <dir>]
#                    [PACKAGED_CONFIG <path>] [EMPTY_PATH])
# Runs under umask 0002 so directories created with default permissions would
# come out group-writable and fail the mode checks below. EMPTY_PATH clears
# PATH for the install script itself.
function(run_install_script case_dir)
  cmake_parse_arguments(
    PARSE_ARGV 1 arg "EMPTY_PATH"
    "EXPECT_FAILURE;EXPECT_OUTPUT;CONFIG_DIR;USER_MODELS_DIR;DESTDIR;PACKAGED_CONFIG" ""
  )
  set(HOWDY_CONFIG_DIR "${case_dir}/root/etc/howdy")
  if(DEFINED arg_CONFIG_DIR)
    set(HOWDY_CONFIG_DIR "${arg_CONFIG_DIR}")
  endif()
  set(HOWDY_CONFIG_PATH "${HOWDY_CONFIG_DIR}/config.ini")
  set(HOWDY_USER_MODELS_DIR "${case_dir}/root/var/lib/howdy/user-models")
  if(DEFINED arg_USER_MODELS_DIR)
    set(HOWDY_USER_MODELS_DIR "${arg_USER_MODELS_DIR}")
  endif()
  set(HOWDY_PACKAGED_CONFIG_PATH "${case_dir}/packaged/config.ini")
  if(DEFINED arg_PACKAGED_CONFIG)
    set(HOWDY_PACKAGED_CONFIG_PATH "${arg_PACKAGED_CONFIG}")
  endif()
  if(DEFINED arg_DESTDIR)
    set(env_args "DESTDIR=${arg_DESTDIR}")
  else()
    set(env_args --unset=DESTDIR)
  endif()
  if(arg_EMPTY_PATH)
    list(APPEND env_args "PATH=")
  endif()

  set(script_path "${case_dir}/install_config.cmake")
  configure_file("${template_path}" "${script_path}" @ONLY)
  execute_process(
    COMMAND sh -c [[umask 0002 && exec "$@"]] sh
            "${CMAKE_COMMAND}" -E env ${env_args} "${CMAKE_COMMAND}" -P "${script_path}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error
  )
  set(combined "${run_output}\n${run_error}")
  if(NOT DEFINED arg_EXPECT_FAILURE)
    if(NOT run_result EQUAL 0)
      message(FATAL_ERROR "Expected success in ${case_dir}, got ${run_result}:\n${combined}")
    endif()
    if(DEFINED arg_EXPECT_OUTPUT)
      string(FIND "${combined}" "${arg_EXPECT_OUTPUT}" output_pos)
      if(output_pos EQUAL -1)
        message(FATAL_ERROR "Expected '${arg_EXPECT_OUTPUT}' in ${case_dir}, got:\n${combined}")
      endif()
    endif()
    return()
  endif()
  if(run_result EQUAL 0)
    message(FATAL_ERROR "Expected failure in ${case_dir}, but succeeded:\n${combined}")
  endif()
  string(FIND "${combined}" "${arg_EXPECT_FAILURE}" fragment_pos)
  if(fragment_pos EQUAL -1)
    message(
      FATAL_ERROR
      "Expected '${arg_EXPECT_FAILURE}' in ${case_dir}, got:\n${combined}"
    )
  endif()
endfunction()

function(prepare_packaged case_dir)
  file(MAKE_DIRECTORY "${case_dir}/packaged")
  file(WRITE "${case_dir}/packaged/config.ini" "[core]\ndisabled = false\n")
endfunction()

function(expect_mode path expected)
  execute_process(
    COMMAND stat -c %a "${path}"
    RESULT_VARIABLE stat_result
    OUTPUT_VARIABLE mode
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(NOT stat_result EQUAL 0 OR NOT mode STREQUAL expected)
    message(FATAL_ERROR "Expected mode ${expected} on ${path}, got '${mode}'")
  endif()
endfunction()

function(expect_regular_config path)
  if(NOT EXISTS "${path}" OR IS_SYMLINK "${path}" OR IS_DIRECTORY "${path}")
    message(FATAL_ERROR "Install did not create regular config.ini at ${path}")
  endif()
endfunction()

# 1. Valid direct install ignores rootfs symlinks beside the walked components
#    (usrmerge style: root/lib next to walked root/etc, root/var/run next to
#    walked root/var/lib), creates new directories with explicit modes despite
#    umask 0002, and preserves an existing config.ini on reinstall.
set(case_ok "${test_root}/ok")
prepare_packaged("${case_ok}")
file(MAKE_DIRECTORY "${case_ok}/root/usr/lib" "${case_ok}/root/run" "${case_ok}/root/var")
file(CREATE_LINK "usr/lib" "${case_ok}/root/lib" SYMBOLIC)
file(CREATE_LINK "../run" "${case_ok}/root/var/run" SYMBOLIC)
run_install_script("${case_ok}")
expect_regular_config("${case_ok}/root/etc/howdy/config.ini")
if(NOT IS_DIRECTORY "${case_ok}/root/var/lib/howdy/user-models")
  message(FATAL_ERROR "Direct install did not create user-models directory")
endif()
expect_mode("${case_ok}/root/etc" 755)
expect_mode("${case_ok}/root/etc/howdy" 750)
expect_mode("${case_ok}/root/var/lib" 755)
expect_mode("${case_ok}/root/var/lib/howdy" 755)
expect_mode("${case_ok}/root/var/lib/howdy/user-models" 750)
file(WRITE "${case_ok}/root/etc/howdy/config.ini" "[core]\ndisabled = true\n")
run_install_script("${case_ok}")
file(READ "${case_ok}/root/etc/howdy/config.ini" preserved_content)
if(NOT preserved_content STREQUAL "[core]\ndisabled = true\n")
  message(FATAL_ERROR "Direct reinstall overwrote existing config.ini")
endif()

# 2. Staged install with a trailing-slash DESTDIR joins without doubling '/'
#    and creates everything inside DESTDIR.
set(case_destdir "${test_root}/destdir")
prepare_packaged("${case_destdir}")
run_install_script(
  "${case_destdir}"
  DESTDIR "${case_destdir}/stage/"
  CONFIG_DIR "/etc/howdy"
  USER_MODELS_DIR "/var/lib/howdy/user-models"
)
expect_regular_config("${case_destdir}/stage/etc/howdy/config.ini")
if(NOT IS_DIRECTORY "${case_destdir}/stage/var/lib/howdy/user-models")
  message(FATAL_ERROR "Staged install did not create user-models inside DESTDIR")
endif()
expect_mode("${case_destdir}/stage/var/lib/howdy" 755)
expect_mode("${case_destdir}/stage/etc/howdy" 750)

# 3. Staged install checks components under DESTDIR, not the host root.
set(case_destdir_link "${test_root}/destdir-symlink")
prepare_packaged("${case_destdir_link}")
file(MAKE_DIRECTORY "${case_destdir_link}/external" "${case_destdir_link}/stage/etc")
file(
  CREATE_LINK
  "${case_destdir_link}/external"
  "${case_destdir_link}/stage/etc/howdy"
  SYMBOLIC
)
run_install_script(
  "${case_destdir_link}"
  EXPECT_FAILURE "destdir-symlink/stage/etc/howdy\n"
  DESTDIR "${case_destdir_link}/stage"
  CONFIG_DIR "/etc/howdy"
  USER_MODELS_DIR "/var/lib/howdy/user-models"
)
if(EXISTS "${case_destdir_link}/external/config.ini")
  message(FATAL_ERROR "Staged install wrote config.ini through symlinked config_dir")
endif()

# 4. Live symlink at config_dir is rejected without writing through it.
set(case_live_dir "${test_root}/live-dir-symlink")
prepare_packaged("${case_live_dir}")
file(MAKE_DIRECTORY "${case_live_dir}/external" "${case_live_dir}/root/etc")
file(CREATE_LINK "${case_live_dir}/external" "${case_live_dir}/root/etc/howdy" SYMBOLIC)
run_install_script("${case_live_dir}" EXPECT_FAILURE "must not be a symlink")
if(EXISTS "${case_live_dir}/external/config.ini")
  message(FATAL_ERROR "Install wrote config.ini through symlinked config_dir")
endif()

# 5. Dangling symlink at config_dir is rejected.
set(case_dangling_dir "${test_root}/dangling-dir-symlink")
prepare_packaged("${case_dangling_dir}")
file(MAKE_DIRECTORY "${case_dangling_dir}/root/etc")
file(
  CREATE_LINK
  "${case_dangling_dir}/missing-target"
  "${case_dangling_dir}/root/etc/howdy"
  SYMBOLIC
)
run_install_script("${case_dangling_dir}" EXPECT_FAILURE "must not be a symlink")

# 6. Symlink ancestor of user_models_dir is rejected before MAKE_DIRECTORY.
set(case_ancestor "${test_root}/ancestor-symlink")
prepare_packaged("${case_ancestor}")
file(MAKE_DIRECTORY "${case_ancestor}/external" "${case_ancestor}/root/var/lib")
file(CREATE_LINK "${case_ancestor}/external" "${case_ancestor}/root/var/lib/howdy" SYMBOLIC)
run_install_script("${case_ancestor}" EXPECT_FAILURE "must not be a symlink")
if(EXISTS "${case_ancestor}/external/user-models")
  message(FATAL_ERROR "Install created user-models through symlinked parent")
endif()

# 7. Symlink ancestor whose name contains '[' is still found: list-based
#    splitting would merge 'a[b' with 'c]d' and inspect a nonexistent path.
set(case_bracket "${test_root}/bracket-component")
prepare_packaged("${case_bracket}")
file(MAKE_DIRECTORY "${case_bracket}/external" "${case_bracket}/root")
file(CREATE_LINK "${case_bracket}/external" "${case_bracket}/root/a[b" SYMBOLIC)
run_install_script(
  "${case_bracket}"
  EXPECT_FAILURE "bracket-component/root/a[b\n"
  CONFIG_DIR "${case_bracket}/root/a[b/c]d/etc/howdy"
)
if(EXISTS "${case_bracket}/external/c]d")
  message(FATAL_ERROR "Install created directories through bracketed symlink")
endif()

# 8. '..' components are rejected before any directory is created.
set(case_dotdot "${test_root}/dotdot")
prepare_packaged("${case_dotdot}")
run_install_script(
  "${case_dotdot}"
  EXPECT_FAILURE "must not contain '..'"
  CONFIG_DIR "${case_dotdot}/root/etc/../howdy"
)
if(EXISTS "${case_dotdot}/root")
  message(FATAL_ERROR "Rejected '..' install left directories behind")
endif()

# 9. A regular file in the middle of the user-models tree is rejected before
#    the config tree (walked first) is created.
set(case_file_component "${test_root}/file-component")
prepare_packaged("${case_file_component}")
file(WRITE "${case_file_component}/root/var/lib/howdy" "not a directory\n")
run_install_script("${case_file_component}" EXPECT_FAILURE "is not a directory")
if(EXISTS "${case_file_component}/root/etc")
  message(FATAL_ERROR "Rejected install created the config tree")
endif()

# 10. Live symlink at config.ini is rejected.
set(case_live_file "${test_root}/live-config-symlink")
prepare_packaged("${case_live_file}")
file(MAKE_DIRECTORY "${case_live_file}/root/etc/howdy")
file(WRITE "${case_live_file}/external.ini" "external\n")
file(
  CREATE_LINK
  "${case_live_file}/external.ini"
  "${case_live_file}/root/etc/howdy/config.ini"
  SYMBOLIC
)
run_install_script("${case_live_file}" EXPECT_FAILURE "must not be a symlink")

# 11. Dangling symlink at config.ini is rejected.
set(case_dangling_file "${test_root}/dangling-config-symlink")
prepare_packaged("${case_dangling_file}")
file(MAKE_DIRECTORY "${case_dangling_file}/root/etc/howdy")
file(
  CREATE_LINK
  "${case_dangling_file}/missing.ini"
  "${case_dangling_file}/root/etc/howdy/config.ini"
  SYMBOLIC
)
run_install_script("${case_dangling_file}" EXPECT_FAILURE "must not be a symlink")
if(EXISTS "${case_dangling_file}/missing.ini")
  message(FATAL_ERROR "Install followed dangling config.ini symlink")
endif()

# 12. Directory at config.ini is rejected.
set(case_dir_at_file "${test_root}/dir-at-config")
prepare_packaged("${case_dir_at_file}")
file(MAKE_DIRECTORY "${case_dir_at_file}/root/etc/howdy/config.ini")
run_install_script("${case_dir_at_file}" EXPECT_FAILURE "is a directory")

# 13. FIFO at config.ini is rejected instead of being kept as the admin config.
set(case_fifo "${test_root}/fifo-config")
prepare_packaged("${case_fifo}")
file(MAKE_DIRECTORY "${case_fifo}/root/etc/howdy")
execute_process(
  COMMAND mkfifo "${case_fifo}/root/etc/howdy/config.ini"
  RESULT_VARIABLE mkfifo_result
)
if(NOT mkfifo_result EQUAL 0)
  message(FATAL_ERROR "mkfifo failed: ${mkfifo_result}")
endif()
run_install_script("${case_fifo}" EXPECT_FAILURE "is not a regular file")

# 14. Symlink at packaged config source is rejected.
set(case_packaged_link "${test_root}/packaged-symlink")
prepare_packaged("${case_packaged_link}")
file(
  CREATE_LINK
  "${case_packaged_link}/packaged/config.ini"
  "${case_packaged_link}/packaged/link.ini"
  SYMBOLIC
)
run_install_script(
  "${case_packaged_link}"
  EXPECT_FAILURE "must not be a symlink"
  PACKAGED_CONFIG "${case_packaged_link}/packaged/link.ini"
)

# 15. Hard-linked existing config.ini is rejected: runtime refuses nlink != 1.
set(case_hardlink "${test_root}/hardlink-config")
prepare_packaged("${case_hardlink}")
file(MAKE_DIRECTORY "${case_hardlink}/root/etc/howdy")
file(WRITE "${case_hardlink}/root/etc/howdy/config.ini" "[core]\n")
file(CREATE_LINK "${case_hardlink}/root/etc/howdy/config.ini" "${case_hardlink}/other.ini")
run_install_script("${case_hardlink}" EXPECT_FAILURE "must not be a hard link")

# 16. A trailing '/.' still gives the real leaf the leaf mode.
set(case_trailing_dot "${test_root}/trailing-dot")
prepare_packaged("${case_trailing_dot}")
run_install_script(
  "${case_trailing_dot}"
  USER_MODELS_DIR "${case_trailing_dot}/root/var/lib/howdy/user-models/."
)
expect_mode("${case_trailing_dot}/root/var/lib/howdy/user-models" 750)
expect_mode("${case_trailing_dot}/root/var/lib/howdy" 755)

# 17. A pre-existing group-writable ancestor (left by an older umask-0002
#     install) is reported, not silently kept or rewritten.
set(case_writable "${test_root}/writable-ancestor")
prepare_packaged("${case_writable}")
file(MAKE_DIRECTORY "${case_writable}/root/var/lib/howdy")
file(CHMOD "${case_writable}/root/var/lib/howdy" PERMISSIONS
     OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_WRITE GROUP_EXECUTE
     WORLD_READ WORLD_EXECUTE)
run_install_script(
  "${case_writable}"
  EXPECT_OUTPUT "writable-ancestor/root/var/lib/howdy\n"
)
expect_mode("${case_writable}/root/var/lib/howdy" 775)

# 18. Reinstall with an empty PATH still inspects config.ini through the fixed
#     stat(1) fallback locations instead of misreporting it.
if(EXISTS "/usr/bin/stat" OR EXISTS "/bin/stat")
  set(case_empty_path "${test_root}/empty-path")
  prepare_packaged("${case_empty_path}")
  run_install_script("${case_empty_path}")
  run_install_script("${case_empty_path}" EMPTY_PATH)
endif()

# 19. A pre-existing group-writable config.ini is kept but reported.
set(case_writable_config "${test_root}/writable-config")
prepare_packaged("${case_writable_config}")
run_install_script("${case_writable_config}")
file(CHMOD "${case_writable_config}/root/etc/howdy/config.ini" PERMISSIONS
     OWNER_READ OWNER_WRITE GROUP_READ GROUP_WRITE)
run_install_script(
  "${case_writable_config}"
  EXPECT_OUTPUT "writable-config/root/etc/howdy/config.ini\n"
)

file(REMOVE_RECURSE "${test_root}")
