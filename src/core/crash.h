#pragma once

// Records a crash to /mnt/extsd/crash.log: signal, fault address, registers,
// the crashing thread, a raw stack dump and the load map, enough to name the
// line with addr2line (utilities/sdcard-test/crash-decode.sh). Also flushes
// the log buffer, which otherwise loses its last second.
void crash_handler_install(void);
