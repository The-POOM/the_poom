// SPDX-License-Identifier: MIT
#include "wardrive_files.h"
#include "poom_wardrive.h"
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
bool wd_archive(const char *directory) {
    char tmp[192],dest[192];
    struct stat journal;
    if(snprintf(tmp,sizeof(tmp),"%s/observations.wd",directory)>=(int)sizeof(tmp)||
       stat(tmp,&journal)||!S_ISREG(journal.st_mode)) return false;
    if(snprintf(tmp,sizeof(tmp),"%s/session.tar.tmp",directory)>=(int)sizeof(tmp)||snprintf(dest,sizeof(dest),"%s/session.tar",directory)>=(int)sizeof(dest)) return false;
    FILE *out=fopen(tmp,"wb"); if(!out) return false;
    static const char *const names[]={"session.json","observations.wd","wigle.csv","recovery.json","upload.json","export.json"};
    bool ok=true; char b[512];
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0])&&ok;i++) {
        char p[192]; struct stat st;
        if(snprintf(p,sizeof(p),"%s/%s",directory,names[i])>=(int)sizeof(p)) { ok=false; break; }
        if(stat(p,&st)) { if(errno==ENOENT) continue; ok=false; break; }
        if(!S_ISREG(st.st_mode)||st.st_size<0||st.st_size>0x7fffffff) { ok=false; break; }
        FILE *in=fopen(p,"rb"); if(!in) { ok=false; break; }
        memset(b,0,sizeof(b)); snprintf(b,100,"%s",names[i]);
        memcpy(b+100,"0000600",7); memcpy(b+108,"0000000",7); memcpy(b+116,"0000000",7);
        snprintf(b+124,12,"%011lo",(unsigned long)st.st_size);
        memcpy(b+136,"00000000000",11); memset(b+148,' ',8); b[156]='0'; memcpy(b+257,"ustar",5); memcpy(b+263,"00",2);
        unsigned sum=0; for(unsigned j=0;j<512;j++) sum+=(unsigned char)b[j];
        snprintf(b+148,7,"%06o",sum); b[155]=' ';
        ok=fwrite(b,1,512,out)==512;
        size_t n,total=0;
        while(ok&&(n=fread(b,1,sizeof(b),in))) { ok=fwrite(b,1,n,out)==n; total+=n; }
        if(ferror(in)||total!=(size_t)st.st_size) ok=false;
        fclose(in); memset(b,0,sizeof(b));
        if(ok&&total%512) ok=fwrite(b,1,512-total%512,out)==512-total%512;
    }
    memset(b,0,sizeof(b));
    if(ok) ok=fwrite(b,1,512,out)==512&&fwrite(b,1,512,out)==512;
    if(fflush(out)||fsync(fileno(out))) ok=false;
    if(fclose(out)) ok=false;
    if(ok&&!wd_file_publish(tmp,dest)) ok=false;
    return ok;
}
