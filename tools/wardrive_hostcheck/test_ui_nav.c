// SPDX-License-Identifier: MIT
#include "wardrive_ui_nav.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    const unsigned pages[]={WD_SCREEN_LIVE,WD_SCREEN_GPS,WD_SCREEN_STATS};
    for(unsigned i=0;i<3;i++) {
        unsigned page=pages[i];
        assert(wd_ui_session_back(page)==WD_SCREEN_MAIN&&wd_ui_is_live_page(page));
        assert(wd_ui_session_page(page,true)==pages[(i+1)%3]);
        assert(wd_ui_session_page(page,false)==pages[(i+2)%3]);
        assert(wd_ui_session_page(wd_ui_session_page(page,true),false)==page);
        unsigned next=page;
        for(unsigned j=0;j<30;j++) next=wd_ui_session_page(next,true);
        assert(next==page);
    }
    assert(wd_ui_session_page(WD_SCREEN_MAIN,true)==WD_SCREEN_MAIN&&!wd_ui_is_live_page(WD_SCREEN_STOP));
    assert(wd_ui_session_back(WD_SCREEN_SAVED)==WD_SCREEN_SAVED);
    for(unsigned page=0;page<3;page++) {
        assert(wd_ui_gps_page(wd_ui_gps_page(page,true),false)==page);
        assert(wd_ui_gps_page(wd_ui_gps_page(page,false),true)==page);
        unsigned next=page,previous=page;
        for(unsigned i=0;i<3;i++) { next=wd_ui_gps_page(next,true); previous=wd_ui_gps_page(previous,false); }
        assert(next==page&&previous==page);
    }
    assert(wd_ui_gps_page(0,false)==2);
    assert(wd_ui_gps_page(2,true)==0);
    puts("Three-page L/R carousel round trips and explicit GPS previous/next navigation passed");
}
