// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdio.h>
/* FAT rename cannot overwrite. Publish a closed/flushed temporary file using
 * a .bak rollback, restoring the old destination if publication fails. */
bool wd_file_publish(const char *temporary,const char *destination);
/* Recover reads during a power cut between old->backup and temp->destination.
 * Never consume the uncommitted temporary file. Preserves errno on failure. */
FILE *wd_file_open_read(const char *path);
