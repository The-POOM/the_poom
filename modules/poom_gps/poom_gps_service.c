// SPDX-License-Identifier: MIT
#include "poom_gps.h"
#include <stdio.h>
#include <string.h>
void poom_gps_service_init(poom_gps_service_t *s,const poom_gps_adapter_t *a)
{
    memset(s,0,sizeof(*s)); s->adapter=a;
    if(a) snprintf(s->parser.fix.receiver,sizeof(s->parser.fix.receiver),"%.23s",a->name);
}
void poom_gps_service_poll(poom_gps_service_t *s,uint64_t now)
{
    if(!s->adapter) return;
    if(!s->opened) {
        if(now<s->retry_ms) return;
        s->retry_ms=now+2000;
        if(!s->adapter->open(s->adapter->context)) return;
        s->opened=true;
    }
    uint8_t bytes[32];
    int n=s->adapter->read(s->adapter->context,bytes,sizeof(bytes));
    if(n<0 || n>(int)sizeof(bytes)) {
        poom_gps_service_close(s); s->retry_ms=now+2000; return;
    }
    for(int i=0;i<n;i++) {
        bool end=(bytes[i]=='\r'||bytes[i]=='\n') && s->parser.length;
        bool valid=poom_nmea_feed(&s->parser,bytes[i],now);
        if(end && s->diagnostic) s->diagnostic(s->parser.sentence,valid,s->diagnostic_context);
    }
}
void poom_gps_service_close(poom_gps_service_t *s)
{
    if(s->opened && s->adapter->close) s->adapter->close(s->adapter->context);
    s->opened=false; s->parser.fix.connected=false; s->parser.fix.fix=false;
    s->parser.fix.fields=0; s->parser.length=0;
}
