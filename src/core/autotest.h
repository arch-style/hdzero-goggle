#pragma once

// Drives the player from /mnt/extsd/autotest.txt after boot, so that one
// boot runs a whole matrix of pauses, steps and resumes. See autotest.c for
// the script's grammar; results come out as "AT ..." lines in the log, which
// utilities/sdcard-test/autotest-report.py turns into a table.
void autotest_start(void);
