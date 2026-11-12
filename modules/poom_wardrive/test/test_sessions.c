// SPDX-License-Identifier: MIT
#include "wardrive_sessions.h"
#include <assert.h>
#include <string.h>
#include <sys/stat.h>

/* Inject a cache allocation failure without changing the allocator globally. */
#include <stdlib.h>
static bool fail_cache_allocation;
void *__real_realloc(void *pointer,size_t bytes);
void *__wrap_realloc(void *pointer,size_t bytes) {
    if(fail_cache_allocation) return NULL;
    return __real_realloc(pointer,bytes);
}
static void manifest(const char *root,const char *id,const char *json) {
    char path[256]; snprintf(path,sizeof(path),"%s/%s",root,id); assert(!mkdir(path,0700));
    snprintf(path,sizeof(path),"%s/%s/session.json",root,id);
    FILE *f=fopen(path,"w"); assert(f); assert(fputs(json,f)>=0); assert(!fclose(f));
}
int main(int argc,char **argv) {
    assert(argc==2); char root[192]; snprintf(root,sizeof(root),"%s/browse",argv[1]); assert(!mkdir(root,0700));
    char id[49];
    assert(wd_session_id(id,1794117600,1,0x12345678));
    assert(!strcmp(id,"20261108T060000Z-12345678"));
    assert(wd_session_id(id,0,1,2)); assert(!strcmp(id,"undated-00000001-00000002"));
    /* Deliberately unordered legacy IDs, including tied timestamps. */
    manifest(root,"ffffffff-old","{\"start_utc\":\"2026-11-07 12:00:00\"}");
    manifest(root,"00000000-new","{\"start_utc\":\"2026-11-08 06:56:43\"}");
    manifest(root,"00000000-newer-tie","{\"start_utc\":\"2026-11-08 06:56:43\"}");
    manifest(root,"missing-time","{\"state\":\"Completed\"}");
    manifest(root,"bad-date","{\"start_utc\":\"2026-02-31 06:56:43\"}");
    manifest(root,"nested","{\"start_utc\":{\"a\":{\"b\":1}}}");
    manifest(root,"html","{\"start_utc\":\"<script>alert(1)</script>\"}");
    wd_session_info_t info;
    assert(wd_session_info(root,"00000000-new",&info));
    assert(!strcmp(info.label,"08.11.26 06:56:43"));
    assert(!strcmp(info.utc,"2026-11-08 06:56:43"));
    assert(!wd_session_info(root,"../bad",&info));
    assert(wd_session_info(root,"bad-date",&info)&&info.epoch<0);
    assert(wd_session_info(root,"nested",&info)&&info.epoch<0);
    assert(wd_session_info(root,"html",&info)&&info.epoch<0&&!strchr(info.label,'<'));
    wd_session_page_t page;
    assert(wd_session_page(root,"",0,&page)&&page.count==7);
    assert(!strcmp(page.entries[0].id,"00000000-newer-tie")&&page.selected_rank==0);
    char cursor[49]; strcpy(cursor,page.entries[0].id);
    for(unsigned i=1;i<7;i++) {
        assert(wd_session_page(root,cursor,1,&page));
        assert(page.selected_rank==i); assert(strcmp(cursor,page.entries[0].id));
        strcpy(cursor,page.entries[0].id);
    }
    assert(wd_session_page(root,cursor,1,&page)&&page.selected_rank==6);
    for(unsigned i=6;i>0;i--) {
        assert(wd_session_page(root,cursor,-1,&page)&&page.selected_rank==i-1);
        strcpy(cursor,page.entries[0].id);
    }
    assert(wd_session_page(root,cursor,-1,&page)&&page.selected_rank==0);
    /* One-pass neighbours walk the same order as the paged reader, both ways. */
    assert(wd_session_page(root,"",0,&page));
    wd_session_info_t walk,next; strcpy(walk.id,page.entries[0].id);
    assert(!wd_session_neighbor(root,walk.id,-1,WD_TZ_UTC,&next)); /* newest end */
    for(unsigned i=1;i<7;i++) {
        assert(wd_session_neighbor(root,walk.id,1,WD_TZ_UTC,&next));
        wd_session_page_t expected; assert(wd_session_page(root,"",0,&expected));
        assert(wd_session_page(root,walk.id,1,&expected)&&!strcmp(expected.entries[0].id,next.id));
        assert(wd_session_neighbor(root,next.id,-1,WD_TZ_UTC,&walk)&&wd_session_neighbor(root,walk.id,1,WD_TZ_UTC,&walk)&&!strcmp(walk.id,next.id));
    }
    assert(!wd_session_neighbor(root,walk.id,1,WD_TZ_UTC,&next)); /* oldest end */
    assert(!wd_session_neighbor(root,"deleted-session",1,WD_TZ_UTC,&next)&&!wd_session_neighbor(root,walk.id,0,WD_TZ_UTC,&next));
    assert(wd_session_page(root,"deleted-session",0,&page)&&page.selected_rank==0);
    /* Many sessions never require a proportional in-memory list. */
    for(unsigned i=0;i<100;i++) {
        snprintf(id,sizeof(id),"extra-%03u",i);
        manifest(root,id,"{\"start_utc\":\"2026-11-06 00:00:00\"}");
    }
    assert(wd_session_page(root,"",0,&page)&&page.count==107);
    assert(!strcmp(page.entries[0].id,"00000000-newer-tie"));
    assert(wd_session_info_local(root,"00000000-new",WD_TZ_SYDNEY,&info));
    assert(!strcmp(info.label,"08.11.26 17:56:43"));
    assert(!strcmp(info.detail,"08.11.26 17:56 AEDT"));
    char text[64]; assert(wd_session_time_text(&info,text,sizeof(text)));
    assert(!strcmp(text,"2026-11-08 17:56:43 AEDT (UTC+11:00)"));
    assert(!strcmp(info.utc,"2026-11-08 06:56:43")); /* original UTC retained */
    manifest(root,"fold-old","{\"start_utc\":\"2026-04-04 15:30:00\",\"display_timezone\":\"Australia/Sydney\"}");
    manifest(root,"fold-new","{\"start_utc\":\"2026-04-04 16:30:00\",\"display_timezone\":\"Australia/Sydney\"}");
    assert(wd_session_info_local(root,"fold-old",WD_TZ_PERTH,&info));
    assert(!strcmp(info.label,"05.04.26 02:30:00")); /* recorded zone overrides viewing fallback */
    assert(!strcmp(info.detail,"05.04.26 02:30 AEDT"));
    assert(wd_session_info_local(root,"fold-new",WD_TZ_PERTH,&info));
    assert(!strcmp(info.label,"05.04.26 02:30:00")&&!strcmp(info.detail,"05.04.26 02:30 AEST")); /* fold told apart by detail + list order */
    assert(wd_session_page_local(root,"fold-new",0,WD_TZ_UTC,&page));
    assert(!strcmp(page.entries[0].id,"fold-new")&&!strcmp(page.entries[1].id,"fold-old"));
    manifest(root,"bad-state","{\"start_utc\":\"2026-11-08 06:56:43\",\"state\":\"<script>alert(1)</script>\",\"observations\":-1}");
    assert(wd_session_info(root,"bad-state",&info)&&!strcmp(info.state,"Unknown")&&!info.count_known);
    /* Prior isolated builds could leave a dated ID with empty manifest UTC.
     * Those existing recordings must sort ahead of yesterday without rewriting. */
    char clock_root[192];snprintf(clock_root,sizeof(clock_root),"%s/clock-browse",argv[1]);assert(!mkdir(clock_root,0700));
    manifest(clock_root,"yesterday","{\"start_utc\":\"2026-11-10 12:00:00\"}");
    manifest(clock_root,"20261111T120000Z-12345678","{\"start_utc\":\"\",\"state\":\"Completed\",\"observations\":42}");
    assert(wd_session_page(clock_root,"",0,&page)&&page.count==2);
    assert(!strcmp(page.entries[0].id,"20261111T120000Z-12345678"));
    assert(!strcmp(page.entries[0].utc,"2026-11-11 12:00:00")&&page.entries[0].observations==42);
    char metadata_path[256];snprintf(metadata_path,sizeof(metadata_path),"%s/%s/session.json",clock_root,page.entries[0].id);
    FILE *unchanged=fopen(metadata_path,"rb");assert(unchanged);char metadata_text[128]={0};
    assert(fread(metadata_text,1,sizeof(metadata_text)-1,unchanged)>0&&!fclose(unchanged));
    assert(strstr(metadata_text,"\"start_utc\":\"\""));
    /* Snapshot preserves the exact ordering and displayed zone, including
     * legacy metadata, undated entries, ties and daylight-saving folds. */
    wd_session_cache_t cache={0};
    assert(wd_session_cache_load(root,WD_TZ_SYDNEY,&cache)&&cache.ready);
    assert(wd_session_page_local(root,"",0,WD_TZ_SYDNEY,&page)&&page.count==cache.count);
    for(unsigned i=0;i<cache.count;i++) {
        wd_session_page_t cached;
        wd_session_cache_page(&cache,cache.entries[i].id,&cached);
        assert(cached.selected_rank==i&&cached.count==cache.count);
        assert(wd_session_page_local(root,cache.entries[i].id,0,WD_TZ_SYDNEY,&page));
        assert(!memcmp(cached.entries,page.entries,sizeof(page.entries)));
    }
    fail_cache_allocation=true;
    assert(!wd_session_cache_load(root,WD_TZ_UTC,&cache));
    assert(!cache.ready&&!cache.entries&&!cache.count);
    fail_cache_allocation=false;
    assert(wd_session_cache_load(root,WD_TZ_UTC,&cache)&&cache.ready);
    wd_session_cache_close(&cache);wd_session_cache_close(&cache);
    assert(!cache.ready&&!cache.entries&&!cache.count);
    /* Going past the memory budget must not silently truncate recordings:
     * reject the snapshot so the unchanged streaming pager can show all. */
    char large[192];snprintf(large,sizeof(large),"%s/large-browser",argv[1]);assert(!mkdir(large,0700));
    unsigned limit=WD_SESSION_CACHE_BYTES/sizeof(wd_session_info_t);
    for(unsigned i=0;i<=limit;i++) {
        char directory[256];snprintf(directory,sizeof(directory),"%s/run-%05u",large,i);assert(!mkdir(directory,0700));
    }
    assert(!wd_session_cache_load(large,WD_TZ_UTC,&cache)&&!cache.ready&&!cache.entries);
    assert(wd_session_page(large,"",0,&page)&&page.count==limit+1);
    char missing[192];snprintf(missing,sizeof(missing),"%s/missing-browser",argv[1]);
    assert(wd_session_cache_load(missing,WD_TZ_UTC,&cache)&&cache.ready&&!cache.count);
    wd_session_cache_page(&cache,"deleted",&page);assert(!page.count&&!page.entries[0].id[0]);
    wd_session_cache_close(&cache);
    puts("Session UTC IDs, legacy labels, newest-first bounded browsing, invalid metadata and navigation passed");
}
