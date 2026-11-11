// SPDX-License-Identifier: MIT
/* Link tests with --wrap=rename: enforce ESP-IDF FatFs FR_EXIST behaviour. */
#include <errno.h>
#include <sys/stat.h>
int wd_test_rename_calls,wd_test_fail_rename_call;
int __real_rename(const char *,const char *);
int __wrap_rename(const char *from,const char *to) {
    if(++wd_test_rename_calls==wd_test_fail_rename_call) { errno=EIO; return -1; }
    struct stat st;
    if(stat(to,&st)==0) { errno=EEXIST; return -1; }
    return __real_rename(from,to);
}
