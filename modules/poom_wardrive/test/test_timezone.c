// SPDX-License-Identifier: MIT
#include "wardrive_timezone.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(int argc,char **argv) {
    if(argc==2&&!strcmp(argv[1],"--zones")) {
        for(unsigned i=0;i<WD_TZ_COUNT;i++) printf("%s\n",wd_timezone_id((wd_timezone_t)i));
        return 0;
    }
    if(argc==2&&!strcmp(argv[1],"--oracle")) {
        unsigned zone; long long utc;
        while(scanf("%u %lld",&zone,&utc)==2) {
            wd_local_time_t local; char text[20];
            assert(wd_timezone_local(utc,(wd_timezone_t)zone,&local));
            assert(strftime(text,sizeof(text),"%Y-%m-%d %H:%M:%S",&local.calendar));
            printf("%d %u %s\n",local.offset_minutes,local.daylight,text);
        }
        return 0;
    }
    wd_timezone_config_t config; wd_timezone_default(&config);
    assert(sizeof(config)==4&&wd_timezone_config_valid(&config));
    config.version=2; assert(!wd_timezone_config_valid(&config));
    config.version=1; config.reserved=1; assert(!wd_timezone_config_valid(&config));
    wd_timezone_default(&config); config.manual_zone=WD_TZ_NEW_YORK;
    wd_timezone_state_t state; wd_timezone_init(&state,&config);
    assert(state.zone==WD_TZ_NEW_YORK&&state.source==WD_TZ_FALLBACK);
    const struct { double lat,lon; wd_timezone_t zone; } cities[]={
        {-33.86,151.2,WD_TZ_SYDNEY},{-37.81,144.96,WD_TZ_MELBOURNE},
        {-42.88,147.32,WD_TZ_HOBART},{-27.47,153.03,WD_TZ_BRISBANE},
        {-34.93,138.6,WD_TZ_ADELAIDE},{-12.46,130.84,WD_TZ_DARWIN},
        {-31.95,115.86,WD_TZ_PERTH},{-31.68,128.88,WD_TZ_EUCLA},
        {-31.55,159.08,WD_TZ_LORD_HOWE},{-29.03,167.95,WD_TZ_NORFOLK},
        {-31.95,141.45,WD_TZ_BROKEN_HILL},{-36.85,174.76,WD_TZ_AUCKLAND},
        {-41.29,174.78,WD_TZ_AUCKLAND},{-43.53,172.63,WD_TZ_AUCKLAND},
        {-43.95,-176.55,WD_TZ_CHATHAM}
    };
    for(unsigned i=0;i<sizeof(cities)/sizeof(cities[0]);i++) {
        wd_timezone_t zone; assert(wd_timezone_detect(cities[i].lat,cities[i].lon,&zone)&&zone==cities[i].zone);
        wd_timezone_t roundtrip; assert(wd_timezone_from_id(wd_timezone_id(zone),&roundtrip)&&roundtrip==zone);
    }
    wd_timezone_t zone;
    assert(!wd_timezone_detect(-28.178,153.54,&zone)); /* Tweed border guard */
    assert(!wd_timezone_detect(-26,138,&zone)); /* state border junction */
    assert(!wd_timezone_detect(-31.8,126.0,&zone)); /* Eucla highway gap */
    assert(!wd_timezone_detect(51.5,-0.1,&zone)); /* outside regional pack */
    assert(!wd_timezone_detect(NAN,1,&zone)&&!wd_timezone_detect(1,INFINITY,&zone));
    assert(!wd_timezone_detect(91,0,&zone)&&!wd_timezone_detect(0,181,&zone));
    poom_gps_fix_t fix={.connected=true,.fix=true,.fields=GPS_POSITION,
        .position_ms=1000,.latitude=-37.81,.longitude=144.96};
    wd_timezone_update(&state,&fix,1000); assert(state.zone==WD_TZ_MELBOURNE&&state.source==WD_TZ_GPS);
    wd_timezone_update(&state,&fix,4000); assert(state.zone==WD_TZ_MELBOURNE&&state.source==WD_TZ_LAST_GPS);
    fix.position_ms=5000; wd_timezone_update(&state,&fix,4000);
    assert(state.zone==WD_TZ_MELBOURNE&&state.source==WD_TZ_LAST_GPS); /* future fix ignored */
    fix.position_ms=4000; fix.connected=false;
    wd_timezone_update(&state,&fix,4000); assert(state.source==WD_TZ_LAST_GPS);
    fix.connected=true;
    fix.latitude=0; fix.longitude=0; fix.position_ms=4000;
    wd_timezone_update(&state,&fix,4000); assert(state.zone==WD_TZ_NEW_YORK&&state.source==WD_TZ_FALLBACK&&!state.have_gps_zone);
    wd_timezone_update(&state,NULL,5000); assert(state.source==WD_TZ_FALLBACK);
    state.config.automatic=0; state.config.manual_zone=WD_TZ_ADELAIDE;
    wd_timezone_init(&state,&state.config); /* alias-safe reconfiguration */
    wd_timezone_update(&state,NULL,5000); assert(state.zone==WD_TZ_ADELAIDE&&state.source==WD_TZ_MANUAL);
    wd_local_time_t local;
    assert(!wd_timezone_local(0,WD_TZ_UTC,&local));
    assert(!wd_timezone_local(1794117600,WD_TZ_COUNT,&local));
    assert(wd_timezone_local(1794117600,WD_TZ_SYDNEY,&local));
    assert(local.calendar.tm_hour==17&&local.offset_minutes==660&&!strcmp(local.abbreviation,"AEDT"));
    puts("Timezone config, conservative regional detection, fallback/manual, stale-fix and offset tests passed");
}
