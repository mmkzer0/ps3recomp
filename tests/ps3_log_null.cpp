/* The run log, left out on purpose.
 *
 * runtime/config/config.cpp calls ps3_log_configure() once the configuration is public, so every program
 * that links config.cpp needs a definition. A title, a CMake fixture and rsx_replay link the real one
 * (runtime/diag/ps3_log.c). The standalone SPU/SPURS fixtures that the Python tools compile with one C++ driver
 * line per source take this instead: they test something else, run without a log.file, and compiling a C
 * file through that line would not work. A fixture that links this configures no log: an invalid log.file
 * does not stop it. Anything that promises the log behaviour must link ps3_log.c (see
 * tests/gates/test_config.py, test_log_file_is_configured_by_a_config_only_consumer). */
extern "C" void ps3_log_configure(void) {}
