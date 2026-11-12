// SPDX-License-Identifier: MIT
#include "wardrive_sessions.h"
#include "cJSON.h"
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>

bool wd_session_id(char out[49],int64_t utc,uint32_t boot_ms,uint32_t random) {
    time_t t=(time_t)utc; struct tm tm;
    if(utc>=1704067200&&utc<=4102444799LL&&gmtime_r(&t,&tm)) {
        char stamp[17];
        if(!strftime(stamp,sizeof(stamp),"%Y%m%dT%H%M%SZ",&tm)) return false;
        snprintf(out,49,"%s-%08lx",stamp,(unsigned long)random);
    } else snprintf(out,49,"undated-%08lx-%08lx",(unsigned long)boot_ms,(unsigned long)random);
    return wd_safe_id(out);
}
static int64_t parse_utc(const char *s) {
    if(!s||strlen(s)!=19) return -1;
    for(unsigned i=0;i<19;i++) {
        char expected=i==4||i==7?'-':i==10?' ':i==13||i==16?':':0;
        if(expected?s[i]!=expected:(s[i]<'0'||s[i]>'9')) return -1;
    }
    poom_gps_fix_t f={.fields=GPS_DATE|GPS_TIME};
    if(sscanf(s,"%u-%u-%u %u:%u:%u",&f.year,&f.month,&f.day,&f.hour,&f.minute,&f.second)!=6) return -1;
    static const unsigned days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(f.month<1||f.month>12||f.day<1||f.day>days[f.month-1]+(f.month==2&&f.year%4==0)) return -1;
    return wd_gps_epoch(&f);
}
/* Earlier isolated builds named sessions with app UTC but left manifest UTC
 * empty. Recover that display/sort time read-only; never rename recordings. */
static int64_t id_utc(const char *id,char utc[20]) {
    if(strlen(id)!=25||id[8]!='T'||id[15]!='Z'||id[16]!='-') return -1;
    snprintf(utc,20,"%.4s-%.2s-%.2s %.2s:%.2s:%.2s",id,id+4,id+6,id+9,id+11,id+13);
    return parse_utc(utc);
}
/* Reject nested input before cJSON: manifests are flat objects. */
static bool flat_object(const char *s) {
    unsigned depth=0; bool quoted=false,escape=false;
    for(;*s;s++) {
        if(quoted) { if(escape) escape=false; else if(*s=='\\') escape=true; else if(*s=='"') quoted=false; }
        else if(*s=='"') quoted=true;
        else if(*s=='['||*s==']') return false;
        else if(*s=='{') { if(++depth>1) return false; }
        else if(*s=='}') { if(!depth) return false; depth--; }
    }
    return !quoted&&!depth;
}
bool wd_session_info_local(const char *root,const char *id,wd_timezone_t fallback,wd_session_info_t *info) {
    if(!wd_safe_id(id)) return false;
    char path[256]; struct stat st;
    int n=snprintf(path,sizeof(path),"%s/%s",root,id);
    if(n<0||n>=(int)sizeof(path)||stat(path,&st)||!S_ISDIR(st.st_mode)) return false;
    memset(info,0,sizeof(*info)); strcpy(info->id,id); info->epoch=-1;
    strcpy(info->state,"Unknown");
    info->display_zone=fallback>=0&&fallback<WD_TZ_COUNT?fallback:WD_TZ_UTC;
    n=snprintf(path,sizeof(path),"%s/%s/session.json",root,id);
    if(n<0||n>=(int)sizeof(path)) return false;
    FILE *f=fopen(path,"rb");
    if(f) {
        char *json=malloc(2049);
        if(json) {
            size_t len=fread(json,1,2048,f); bool complete=!ferror(f)&&fgetc(f)==EOF;
            json[len]=0;
            cJSON *object=complete&&flat_object(json)?cJSON_ParseWithLengthOpts(json,len+1,NULL,true):NULL;
            const cJSON *utc=cJSON_GetObjectItemCaseSensitive(object,"start_utc");
            if(cJSON_IsObject(object)&&cJSON_IsString(utc)&&
               (info->epoch=parse_utc(utc->valuestring))>=0)
                snprintf(info->utc,sizeof(info->utc),"%.19s",utc->valuestring);
            const cJSON *zone=cJSON_GetObjectItemCaseSensitive(object,"display_timezone");
            if(cJSON_IsString(zone)&&!wd_timezone_from_id(zone->valuestring,&info->display_zone)) info->display_zone=WD_TZ_UTC;
            const cJSON *count=cJSON_GetObjectItemCaseSensitive(object,"observations");
            if(cJSON_IsNumber(count)&&count->valuedouble>=0&&count->valuedouble<=UINT32_MAX&&floor(count->valuedouble)==count->valuedouble) {
                info->observations=(uint32_t)count->valuedouble; info->count_known=true;
            }
            const cJSON *state=cJSON_GetObjectItemCaseSensitive(object,"state");
            static const char *const states[]={"Active","Paused","Completed","NeedsRecovery"};
            if(cJSON_IsString(state)) for(unsigned i=0;i<4;i++) if(!strcmp(state->valuestring,states[i])) strcpy(info->state,states[i]);
            cJSON_Delete(object); free(json);
        }
        fclose(f);
    }
    if(info->epoch<0) {
        char recovered[20];int64_t epoch=id_utc(id,recovered);
        if(epoch>=0) { info->epoch=epoch;snprintf(info->utc,sizeof(info->utc),"%s",recovered); }
    }
    if(info->epoch>=0) {
        wd_local_time_t local;
        if(wd_timezone_local(info->epoch,info->display_zone,&local)) {
            /* List rows: DD.MM.YY plus seconds to separate same-day runs.
             * The zone abbreviation is shown on the session detail line. */
            strftime(info->label,sizeof(info->label),"%d.%m.%y %H:%M:%S",&local.calendar);
            char date[15]; strftime(date,sizeof(date),"%d.%m.%y %H:%M",&local.calendar);
            snprintf(info->detail,sizeof(info->detail),"%s %.5s",date,local.abbreviation);
        }
    }
    if(!info->label[0]) {
        size_t len=strlen(id);
        snprintf(info->label,sizeof(info->label),"NO UTC %.8s",id+(len>8?len-8:0));
    }
    return true;
}
bool wd_session_info(const char *root,const char *id,wd_session_info_t *info) {
    return wd_session_info_local(root,id,WD_TZ_UTC,info);
}
bool wd_session_time_text(const wd_session_info_t *info,char *out,size_t capacity) {
    wd_local_time_t local;
    if(!wd_timezone_local(info->epoch,info->display_zone,&local)) return false;
    char date[20];
    if(!strftime(date,sizeof(date),"%Y-%m-%d %H:%M:%S",&local.calendar)) return false;
    unsigned minutes=(unsigned)abs(local.offset_minutes);
    int n=snprintf(out,capacity,"%s %s (UTC%c%02u:%02u)",date,local.abbreviation,local.offset_minutes<0?'-':'+',minutes/60,minutes%60);
    return n>0&&(size_t)n<capacity;
}
static int compare(const wd_session_info_t *a,const wd_session_info_t *b) {
    if(a->epoch!=b->epoch) return a->epoch>b->epoch?-1:1;
    return strcmp(b->id,a->id);
}
bool wd_session_page_local(const char *root,const char *cursor,int direction,wd_timezone_t fallback,wd_session_page_t *page) {
    memset(page,0,sizeof(*page));
    wd_session_info_t current,candidate,entry;
    bool anchored=cursor&&*cursor&&wd_session_info_local(root,cursor,fallback,&current);
    bool found=anchored; if(anchored) candidate=current;
    bool neighbor=false;
    DIR *d=opendir(root); if(!d) return errno==ENOENT;
    struct dirent *e;
    while((e=readdir(d))) {
        if(!wd_session_info_local(root,e->d_name,fallback,&entry)) continue;
        page->count++;
        if(!anchored) { if(!found||compare(&entry,&candidate)<0) { candidate=entry; found=true; } }
        else if(direction) {
            int side=compare(&entry,&current);
            bool eligible=direction<0?side<0:side>0;
            if(eligible&&(!neighbor||(direction<0?compare(&entry,&candidate)>0:compare(&entry,&candidate)<0))) {
                candidate=entry; neighbor=true;
            }
        }
    }
    closedir(d); if(!found) return true;
    d=opendir(root); if(!d) return false;
    while((e=readdir(d))) {
        if(!wd_session_info_local(root,e->d_name,fallback,&entry)) continue;
        if(compare(&entry,&candidate)<0) { page->selected_rank++; continue; }
        for(unsigned i=0;i<4;i++) {
            if(!page->entries[i].id[0]||compare(&entry,&page->entries[i])<0) {
                for(unsigned j=3;j>i;j--) page->entries[j]=page->entries[j-1];
                page->entries[i]=entry; break;
            }
        }
    }
    closedir(d); return true;
}
bool wd_session_neighbor(const char *root,const char *cursor,int direction,wd_timezone_t fallback,wd_session_info_t *out) {
    wd_session_info_t current,entry; bool found=false;
    if(!direction||!wd_session_info_local(root,cursor,fallback,&current)) return false;
    DIR *d=opendir(root); if(!d) return false;
    struct dirent *e;
    while((e=readdir(d))) {
        if(!wd_session_info_local(root,e->d_name,fallback,&entry)) continue;
        int side=compare(&entry,&current);
        if(direction<0?side>=0:side<=0) continue;
        if(!found||(direction<0?compare(&entry,out)>0:compare(&entry,out)<0)) { *out=entry; found=true; }
    }
    closedir(d); return found;
}
bool wd_session_page(const char *root,const char *cursor,int direction,wd_session_page_t *page) {
    return wd_session_page_local(root,cursor,direction,WD_TZ_UTC,page);
}

void wd_session_cache_close(wd_session_cache_t *cache) {
    free(cache->entries); memset(cache,0,sizeof(*cache));
}
static int cache_compare(const void *a,const void *b) { return compare(a,b); }
bool wd_session_cache_load(const char *root,wd_timezone_t fallback,wd_session_cache_t *cache) {
    wd_session_cache_close(cache);
    DIR *directory=opendir(root);
    if(!directory) { cache->ready=errno==ENOENT; return cache->ready; }
    const unsigned limit=WD_SESSION_CACHE_BYTES/sizeof(wd_session_info_t);
    unsigned capacity=0;
    struct dirent *item;
    while((item=readdir(directory))) {
        wd_session_info_t entry;
        if(!wd_session_info_local(root,item->d_name,fallback,&entry)) continue;
        if(cache->count==capacity) {
            unsigned next=capacity?capacity*2:32;
            if(next>limit) next=limit;
            if(next<=capacity) goto failed;
            void *allocation=realloc(cache->entries,next*sizeof(*cache->entries));
            if(!allocation) goto failed;
            cache->entries=allocation; capacity=next;
        }
        cache->entries[cache->count++]=entry;
    }
    closedir(directory);
    if(cache->count>1) qsort(cache->entries,cache->count,sizeof(*cache->entries),cache_compare);
    cache->ready=true; return true;
failed:
    closedir(directory); wd_session_cache_close(cache); return false;
}
void wd_session_cache_page(const wd_session_cache_t *cache,const char *cursor,wd_session_page_t *page) {
    memset(page,0,sizeof(*page));
    if(!cache->ready) return;
    page->count=cache->count;
    unsigned first=0;
    if(cursor&&*cursor) for(unsigned i=0;i<cache->count;i++)
        if(!strcmp(cache->entries[i].id,cursor)) { first=i; break; }
    page->selected_rank=first;
    for(unsigned i=0;i<4&&first+i<cache->count;i++) page->entries[i]=cache->entries[first+i];
}
