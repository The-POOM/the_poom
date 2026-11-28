// SPDX-License-Identifier: MIT
#include "wardrive_files.h"
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static bool backup_name(char out[256],const char *path) {
    int n=snprintf(out,256,"%s.bak",path);
    if(n<0||n>=256) { errno=ENAMETOOLONG; return false; }
    return true;
}
static bool failed(const char *stage,const char *path) {
    int error=errno;
    printf("[wardrive] SD publish %s errno=%d file=%.191s\n",stage,error,path);
    errno=error; return false;
}
bool wd_file_publish(const char *temporary,const char *destination) {
    char backup[256]; struct stat st;
    if(!backup_name(backup,destination)) return false;
    if(stat(temporary,&st)) return failed("stat-temporary",temporary);
    bool exists=stat(destination,&st)==0;
    if(!exists&&errno!=ENOENT) return failed("stat-destination",destination);
    if(!exists) {
        /* Recover an interrupted previous publication before attempting a new
         * one. Never discard the sole surviving committed file. */
        if(stat(backup,&st)==0) {
            if(rename(backup,destination)) return failed("restore-backup",destination);
            exists=true;
        } else if(errno!=ENOENT) return failed("stat-backup",backup);
    }
    if(exists) {
        if(unlink(backup)&&errno!=ENOENT) return failed("remove-old-backup",backup);
        if(rename(destination,backup)) return failed("backup-destination",destination);
    }
    if(rename(temporary,destination)) {
        int error=errno;
        if(exists&&rename(backup,destination)) failed("rollback",destination);
        errno=error; return failed("install-temporary",destination);
    }
    /* A committed destination now exists. Stale backup cleanup isn't grounds
     * for reporting a failed publication (especially after an upload). */
    if(exists&&unlink(backup)&&errno!=ENOENT) failed("cleanup-backup",backup);
    return true;
}
FILE *wd_file_open_read(const char *path) {
    FILE *f=fopen(path,"rb");
    if(f||errno!=ENOENT) return f;
    char backup[256]; if(!backup_name(backup,path)) return NULL;
    return fopen(backup,"rb");
}
