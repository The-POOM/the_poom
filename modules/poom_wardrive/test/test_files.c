// SPDX-License-Identifier: MIT
#include "wardrive_files.h"
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
extern int wd_test_rename_calls,wd_test_fail_rename_call;
static void write_text(const char *p,const char *text) {
    FILE *f=fopen(p,"w"); assert(f); assert(fputs(text,f)>=0); assert(!fclose(f));
}
static void check(const char *p,const char *expected) {
    FILE *f=wd_file_open_read(p); char b[32]={0}; assert(f);
    assert(fread(b,1,sizeof(b)-1,f)==strlen(expected)); fclose(f); assert(!strcmp(b,expected));
}
int main(int argc,char **argv) {
    assert(argc==2); char dest[256],tmp[256],backup[256];
    snprintf(dest,sizeof(dest),"%s/state",argv[1]); snprintf(tmp,sizeof(tmp),"%s/state.tmp",argv[1]); snprintf(backup,sizeof(backup),"%s/state.bak",argv[1]);
    write_text(tmp,"first"); assert(wd_file_publish(tmp,dest)); check(dest,"first");
    write_text(tmp,"second"); assert(rename(tmp,dest)==-1&&errno==EEXIST);
    assert(wd_file_publish(tmp,dest)); check(dest,"second");
    write_text(tmp,"third"); wd_test_fail_rename_call=wd_test_rename_calls+2;
    assert(!wd_file_publish(tmp,dest)); check(dest,"second"); check(tmp,"third");
    wd_test_fail_rename_call=0;
    assert(rename(dest,backup)==0); /* power cut before installing new file */
    check(dest,"second");
    assert(wd_file_publish(tmp,dest)); check(dest,"third"); assert(access(backup,F_OK)!=0);
    assert(rename(dest,backup)==0);
    unlink(tmp); assert(!wd_file_publish(tmp,dest)); check(dest,"third");
    puts("FAT-compatible publication: replace, injected I/O failure, rollback and interrupted-read recovery passed");
}
