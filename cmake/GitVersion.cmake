# Regenerates build_info.hpp from the current git state.
#
# Run in script mode (cmake -P) from a build-time custom target, not only at
# configure time: the whole point of the stamp is that a binary reports the
# commit it was actually built from, and a configure-time-only capture goes
# stale the moment someone commits and types `make`.
#
# Expects: GIT_SRC_DIR (repo working tree), OUT_FILE (header to write),
#          IN_FILE (template).

find_package(Git QUIET)

set(EDSPARSER_GIT_COMMIT "unknown")
set(EDSPARSER_GIT_DATE "unknown")
set(EDSPARSER_GIT_DIRTY "0")

if(GIT_FOUND AND EXISTS "${GIT_SRC_DIR}")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
        WORKING_DIRECTORY "${GIT_SRC_DIR}"
        OUTPUT_VARIABLE _commit
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _commit_rc
    )
    if(_commit_rc EQUAL 0 AND _commit)
        set(EDSPARSER_GIT_COMMIT "${_commit}")
    endif()

    # Committer date, ISO-8601, UTC. Sortable as a plain string, which is what
    # lets a shell guard compare it against a cutoff without a date parser.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" log -1 --format=%cd --date=format-local:%Y-%m-%dT%H:%M:%SZ
        WORKING_DIRECTORY "${GIT_SRC_DIR}"
        OUTPUT_VARIABLE _date
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _date_rc
    )
    if(_date_rc EQUAL 0 AND _date)
        set(EDSPARSER_GIT_DATE "${_date}")
    endif()

    # Uncommitted changes to tracked files mean the commit alone does not
    # identify the binary — record it so results can be marked accordingly.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
        WORKING_DIRECTORY "${GIT_SRC_DIR}"
        OUTPUT_VARIABLE _status
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_status)
        set(EDSPARSER_GIT_DIRTY "1")
    endif()
endif()

# configure_file leaves the file untouched when the contents are unchanged, so
# this does not trigger a rebuild of everything on every make.
configure_file("${IN_FILE}" "${OUT_FILE}" @ONLY)
