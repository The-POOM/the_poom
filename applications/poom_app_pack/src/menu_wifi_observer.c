// SPDX-License-Identifier: GPL-3.0-or-later
/* Native passive GUARD and RECON views share one bounded receive owner. */
#include "menu_wifi_guard.h"
#include "menu_fox_hunt.h"
#include "poom_wifi_observer.h"
#include "Arduboy2.h"
#include "button_driver.h"
#include "input_events.h"
#include "poom_sbus.h"
#include "sd_card.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

typedef enum { ROW_AP, ROW_STATION, ROW_PROBE, ROW_ACTIVITY } row_kind_t;
typedef struct { row_kind_t kind; uint8_t index; } row_t;
typedef struct {
    poom_wifi_observer_t *observer;
    QueueHandle_t buttons;
    uint8_t page, selected, row_count;
    row_t rows[POOM_OBSERVER_APS + POOM_OBSERVER_PROBES + POOM_OBSERVER_ACTIVITY_COUNT], detail;
    bool recon, details, names, focused, subscribed;
    esp_err_t error;
    uint32_t notice_until;
    char notice[22];
} observer_ui_t;
static observer_ui_t *s_ui;
static portMUX_TYPE s_input_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_input_queue;
static bool s_stop_requested;
_Static_assert(POOM_OBSERVER_APS+POOM_OBSERVER_PROBES+POOM_OBSERVER_ACTIVITY_COUNT >= POOM_OBSERVER_STATIONS,"Rows must also fit every station");
static const char s_user[] = "wifi_observer";
static const char *const s_guard_pages[] = {"WIFI GUARD", "PWNAGOTCHI", "PINE-LIKE", "MULTI SSID", "FLOCK CANDIDATES", "ALL ALERTS", "SAVE REPORT"};
static const char *const s_recon_pages[] = {"WIFI RECON", "ACCESS POINTS", "STATIONS", "PROBES", "SAVE REPORT"};
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
bool menu_wifi_observer_is_active(void) {
    portENTER_CRITICAL(&s_input_lock);const bool active=s_ui!=NULL;portEXIT_CRITICAL(&s_input_lock);return active;
}
bool menu_wifi_observer_stop(void) {
    portENTER_CRITICAL(&s_input_lock);s_stop_requested=true;portEXIT_CRITICAL(&s_input_lock);
    for(unsigned i=0;i<500U && menu_wifi_observer_is_active();++i)vTaskDelay(pdMS_TO_TICKS(10U));
    return !menu_wifi_observer_is_active();
}
static void resume_menu(void) { const uint8_t token=1U; (void)poom_sbus_publish("poom/menu/resume", &token, sizeof(token), 0U); }
static void line(unsigned y, const char *text) {
    char clipped[22]; snprintf(clipped, sizeof(clipped), "%.21s", text);
    poom_arduboy_set_cursor(0, y); (void)poom_arduboy_print(clipped);
}
static void mac_text(char *out, size_t capacity, const uint8_t *mac) {
    snprintf(out,capacity,"%02X:%02X:%02X:%02X:%02X:%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}
static void input(const poom_sbus_msg_t *msg, void *user) {
    (void)user;
    if(!msg || msg->len < sizeof(button_event_msg_t)) return;
    button_event_msg_t event; memcpy(&event,msg->data,sizeof(event));
    if(event.event != BUTTON_SINGLE_CLICK) return;
    portENTER_CRITICAL(&s_input_lock);
    if(s_input_queue) (void)xQueueSend(s_input_queue,&event.button,0U);
    portEXIT_CRITICAL(&s_input_lock);
}
static void notice(observer_ui_t *s, const char *message) {
    snprintf(s->notice,sizeof(s->notice),"%.21s",message); s->notice_until=now_ms()+2000U;
}
static bool save(observer_ui_t *s) {
    if(sd_card_is_not_mounted()) { sd_card_begin(); if(sd_card_mount()!=ESP_OK) return false; }
    const char *dir = s->recon ? "/sdcard/wifi_recon" : "/sdcard/wifi_guard";
    if(sd_card_create_dir(dir)!=ESP_OK) return false;
    char path[96];int descriptor=-1;const uint32_t stamp=now_ms();
    for(unsigned sequence=0;sequence<256U;++sequence) {
        snprintf(path,sizeof(path),"%s/session_%010lu_%03u.jsonl",dir,(unsigned long)stamp,sequence);
        descriptor=open(path,O_WRONLY|O_CREAT|O_EXCL,0666);
        if(descriptor>=0)break;
        if(errno!=EEXIST)return false;
    }
    if(descriptor<0)return false;
    FILE *file=fdopen(descriptor,"w");if(!file){close(descriptor);return false;}
    const bool ok=poom_observer_model_write_report(poom_wifi_observer_model(s->observer),file,now_ms(),
        poom_wifi_observer_dropped(s->observer),poom_wifi_observer_tune_errors(s->observer));
    return fclose(file)==0 && ok;
}
static const char *marker(const poom_observer_ap_t *a) {
    if(a->flags & POOM_OBSERVER_PWNAGOTCHI) return "PWN JSON";
    if(a->flags & POOM_OBSERVER_MULTI_SSID) return "MULTI SSID";
    if(a->flags & POOM_OBSERVER_SECURITY_CONFLICT) return "SEC CONFLICT";
    if(a->flags & POOM_OBSERVER_PINE_CONFIRMED) return "PINE HEURISTIC";
    if(a->flags & POOM_OBSERVER_FLOCK_NAME) return "FLOCK NAME CANDIDATE";
    if(a->flags & POOM_OBSERVER_FLOCK_OUI) return "SHARED OUI CANDIDATE";
    return "PINE CANDIDATE";
}
static void collect(observer_ui_t *s) {
    const poom_observer_model_t *m=poom_wifi_observer_model(s->observer);
    s->row_count=0;
    if(!m || !s->page) return;
    if(!s->recon || s->page==1U) {
        for(uint8_t i=0;i<POOM_OBSERVER_APS;++i) {
            const poom_observer_ap_t *a=&m->aps[i]; bool include=s->recon;
            if(!s->recon) {
                if(s->page==1U) include=(a->flags&POOM_OBSERVER_PWNAGOTCHI)!=0;
                else if(s->page==2U) include=(a->flags&(POOM_OBSERVER_PINE_OUI|POOM_OBSERVER_PINE_PATTERN))!=0;
                else if(s->page==3U) include=(a->flags&POOM_OBSERVER_MULTI_SSID)!=0;
                else if(s->page==4U) include=(a->flags&(POOM_OBSERVER_FLOCK_NAME|POOM_OBSERVER_FLOCK_OUI))!=0;
                else if(s->page==5U) include=a->flags!=0;
            }
            if(a->used && include) s->rows[s->row_count++]=(row_t){ROW_AP,i};
        }
        if(!s->recon && (s->page==4U || s->page==5U)) for(uint8_t i=0;i<POOM_OBSERVER_PROBES;++i)
            if(m->probes[i].used && m->probes[i].flags) s->rows[s->row_count++]=(row_t){ROW_PROBE,i};
        if(!s->recon && s->page==5U) for(uint8_t i=0;i<POOM_OBSERVER_ACTIVITY_COUNT;++i)
            if(poom_observer_activity_alert(&m->activity[i],(poom_observer_activity_kind_t)i,now_ms()))
                s->rows[s->row_count++]=(row_t){ROW_ACTIVITY,i};
    } else if(s->page==2U) {
        for(uint8_t i=0;i<POOM_OBSERVER_STATIONS;++i) if(m->stations[i].used) s->rows[s->row_count++]=(row_t){ROW_STATION,i};
    } else if(s->page==3U) {
        for(uint8_t i=0;i<POOM_OBSERVER_PROBES;++i) if(m->probes[i].used) s->rows[s->row_count++]=(row_t){ROW_PROBE,i};
    }
    if(s->selected>=s->row_count) s->selected=0;
}
static uint8_t detail_channel(const observer_ui_t *s) {
    const poom_observer_model_t *m=poom_wifi_observer_model(s->observer);
    switch(s->detail.kind) {
        case ROW_AP:return m->aps[s->detail.index].channel;
        case ROW_STATION:return m->stations[s->detail.index].channel;
        case ROW_PROBE:return m->probes[s->detail.index].channel;
        case ROW_ACTIVITY:return m->activity[s->detail.index].channel;
    }
    return 0;
}
static void detail_draw(const observer_ui_t *s, const poom_observer_model_t *m) {
    char text[64],mac[18];
    if(s->detail.kind==ROW_AP) {
        const poom_observer_ap_t *a=&m->aps[s->detail.index];
        line(0,a->ssid[0]?a->ssid:"HIDDEN AP"); mac_text(mac,sizeof(mac),a->bssid); line(9,mac);
        if(s->names && a->name_count) {
            for(uint8_t i=0;i<a->name_count;++i) line(19U+9U*i,a->names[i]);
            line(46,"U/D:INFO L:FOX HUNT");
        } else {
            snprintf(text,sizeof(text),"CH%u %ddB %s",a->channel,a->rssi,poom_observer_security_name(a->security)); line(19,text);
            if(a->flags & POOM_OBSERVER_PWNAGOTCHI) {
                snprintf(text,sizeof(text),"PWN %.16s",a->pwn_name); line(28,text);
                snprintf(text,sizeof(text),"PWND:%lu V:%.8s",(unsigned long)a->pwn_total,a->pwn_version); line(37,text);
            } else {line(28,a->flags?marker(a):"OBSERVED AP"); snprintf(text,sizeof(text),"NAMES:%u SEEN:%lu",a->name_count,(unsigned long)a->observations);line(37,text);}
            line(46,"U/D:NAMES L:FOX HUNT");
        }
    } else if(s->detail.kind==ROW_STATION) {
        const poom_observer_station_t *a=&m->stations[s->detail.index];
        line(0,"STATION RELATION"); mac_text(mac,sizeof(mac),a->mac);line(9,mac);
        if(a->has_ap) {mac_text(mac,sizeof(mac),a->bssid);line(19,mac);} else line(19,"AP NOT OBSERVED");
        snprintf(text,sizeof(text),"CH%u %ddB %s",a->channel,a->rssi,a->active?"SEEN":"STALE");line(28,text);
        snprintf(text,sizeof(text),"AP CHANGES:%lu",(unsigned long)a->relationship_changes);line(37,text);
        snprintf(text,sizeof(text),"FRAMES:%lu",(unsigned long)a->observations);line(46,text);
    } else if(s->detail.kind==ROW_PROBE) {
        const poom_observer_probe_t *a=&m->probes[s->detail.index];
        line(0,a->ssid[0]?a->ssid:"WILDCARD PROBE");mac_text(mac,sizeof(mac),a->mac);line(9,mac);
        snprintf(text,sizeof(text),"CH%u %ddB",a->channel,a->rssi);line(19,text);
        snprintf(text,sizeof(text),"REPEATS:%lu",(unsigned long)(a->count-1U));line(28,text);
        snprintf(text,sizeof(text),"LAST:%lus AGO",(unsigned long)((now_ms()-a->last_ms)/1000U));line(37,text);
        line(46,a->flags&POOM_OBSERVER_FLOCK_NAME?"FLOCK NAME CANDIDATE":a->flags&POOM_OBSERVER_FLOCK_OUI?"SHARED OUI CANDIDATE":"PASSIVE OBSERVATION");
    } else {
        const poom_observer_activity_t *a=&m->activity[s->detail.index];
        line(0,poom_observer_activity_name((poom_observer_activity_kind_t)s->detail.index));
        mac_text(mac,sizeof(mac),a->bssid);line(9,mac);
        snprintf(text,sizeof(text),"WINDOW:%lu / 5s",(unsigned long)a->window_count);line(19,text);
        snprintf(text,sizeof(text),"SESSION:%lu CH%u",(unsigned long)a->count,a->channel);line(28,text);
        line(37,"PATTERN, NOT PROOF");line(46,"COUNTS ACROSS APS");
    }
    line(56,s->focused?"A:HOP R:SAVE B:BACK":"A:LOCK R:SAVE B:BACK");
}
static void draw(observer_ui_t *s) {
    char text[64]; poom_arduboy_clear(); poom_arduboy_set_text_size(1);
    if(s->error!=ESP_OK) {
        line(0,s->recon?"WIFI RECON":"WIFI GUARD");line(19,"RADIO NOT READY");line(28,esp_err_to_name(s->error));line(56,"A:RETRY B:BACK");
    } else {
        const poom_observer_model_t *m=poom_wifi_observer_model(s->observer);
        if(s->details) detail_draw(s,m);
        else {
            line(0,s->recon?s_recon_pages[s->page]:s_guard_pages[s->page]);
            if(!s->page) {
                snprintf(text,sizeof(text),"AP %u/%u STA %u/%u",m->active_aps,m->ap_count,m->active_stations,m->station_count);line(10,text);
                snprintf(text,sizeof(text),"CH%u RX:%lu DROP:%lu",poom_wifi_observer_channel(s->observer),(unsigned long)m->frames,(unsigned long)poom_wifi_observer_dropped(s->observer));line(19,text);
                snprintf(text,sizeof(text),"JOINS:%lu LEAVES:%lu",(unsigned long)(m->ap_joins+m->station_joins),(unsigned long)(m->ap_departures+m->station_departures));line(28,text);
                snprintf(text,sizeof(text),"PROBES:%lu REPEAT:%lu",(unsigned long)m->probe_count,(unsigned long)m->probe_repeats);line(37,text);
                snprintf(text,sizeof(text),"AP CHANGES:%lu",(unsigned long)m->relationship_changes);line(46,text);
            } else if(s->page==(s->recon?4U:6U)) {
                line(10,"A:SAVE SESSION TO SD");line(19,s->recon?"/wifi_recon/*.jsonl":"/wifi_guard/*.jsonl");
                snprintf(text,sizeof(text),"FULL AP:%lu STA:%lu",(unsigned long)m->ap_overflow,(unsigned long)m->station_overflow);line(28,text);
                snprintf(text,sizeof(text),"PROBE FULL:%lu",(unsigned long)m->probe_overflow);line(37,text);
                snprintf(text,sizeof(text),"TUNE SKIPS:%lu",(unsigned long)poom_wifi_observer_tune_errors(s->observer));line(46,text);
            } else if(!s->row_count) {line(19,"WAITING FOR FRAMES");line(37,"L/R:OTHER VIEWS");}
            else {
                snprintf(text,sizeof(text),"%u/%u CH%u %s",s->selected+1U,s->row_count,poom_wifi_observer_channel(s->observer),s->focused?"LOCK":"HOP");line(10,text);
                const uint8_t first=(s->selected/4U)*4U;
                for(uint8_t i=first;i<s->row_count && i<first+4U;++i) {
                    const row_t row=s->rows[i];char label[48];
                    if(row.kind==ROW_AP) {const poom_observer_ap_t *a=&m->aps[row.index];snprintf(label,sizeof(label),"%s %ddB",a->ssid[0]?a->ssid:"HIDDEN",a->rssi);}
                    else if(row.kind==ROW_STATION) mac_text(label,sizeof(label),m->stations[row.index].mac);
                    else if(row.kind==ROW_PROBE) snprintf(label,sizeof(label),"%s x%lu",m->probes[row.index].ssid[0]?m->probes[row.index].ssid:"WILDCARD",(unsigned long)m->probes[row.index].count);
                    else snprintf(label,sizeof(label),"%s",poom_observer_activity_name((poom_observer_activity_kind_t)row.index));
                    snprintf(text,sizeof(text),"%c%.20s",i==s->selected?'>':' ',label);line(19U+(i-first)*9U,text);
                }
            }
            line(56,"L/R:VIEW A:INFO B:END");
        }
    }
    if((int32_t)(s->notice_until-now_ms())>0) {poom_arduboy_fill_rect(0,46,128,9,0);line(46,s->notice);}
    poom_arduboy_display();
}
static void task(void *arg) {
    observer_ui_t *s=arg; bool running=true, hunt=false;
    uint8_t hunt_bssid[6]={0}, hunt_channel=0; char hunt_name[33]={0};
    s->error=poom_wifi_observer_start(&s->observer);
    while(running) {
        portENTER_CRITICAL(&s_input_lock);const bool stop=s_stop_requested;portEXIT_CRITICAL(&s_input_lock);
        if(stop){hunt=false;break;}
        if(s->observer) {poom_wifi_observer_poll(s->observer);collect(s);}
        uint8_t button;
        if(xQueueReceive(s->buttons,&button,pdMS_TO_TICKS(30))==pdTRUE) {
            if(button==BUTTON_B) {if(s->details) {s->details=false;s->names=false;} else running=false;}
            else if(s->error!=ESP_OK) {if(button==BUTTON_A) {s->error=poom_wifi_observer_stop(&s->observer);if(s->error==ESP_OK)s->error=poom_wifi_observer_start(&s->observer);}}
            else if(s->details) {
                if(button==BUTTON_A) {
                    const esp_err_t error=poom_wifi_observer_focus(s->observer,s->focused?0U:detail_channel(s));
                    if(error==ESP_OK) s->focused=!s->focused;else notice(s,"CHANNEL REJECTED");
                } else if(button==BUTTON_RIGHT) notice(s,save(s)?"REPORT SAVED":"SD WRITE FAILED");
                else if(button==BUTTON_LEFT && s->detail.kind==ROW_AP) {
                    const poom_observer_ap_t *a=&poom_wifi_observer_model(s->observer)->aps[s->detail.index];
                    if(poom_wifi_observer_focus(s->observer,a->channel)!=ESP_OK) notice(s,"CHANNEL REJECTED");
                    else {
                        memcpy(hunt_bssid,a->bssid,6U);memcpy(hunt_name,a->ssid,sizeof(hunt_name));hunt_channel=a->channel;
                        hunt=true;running=false;
                    }
                }
                else if(button==BUTTON_UP || button==BUTTON_DOWN) s->names=!s->names;
            } else if(button==BUTTON_LEFT || button==BUTTON_RIGHT) {
                const uint8_t pages=s->recon?5U:7U;
                s->page=(s->page+(button==BUTTON_LEFT?pages-1U:1U))%pages;s->selected=0;
            } else if(button==BUTTON_UP || button==BUTTON_DOWN) {
                if(s->row_count) s->selected=(s->selected+(button==BUTTON_UP?s->row_count-1U:1U))%s->row_count;
            } else if(button==BUTTON_A) {
                if(s->page==(s->recon?4U:6U)) notice(s,save(s)?"REPORT SAVED":"SD WRITE FAILED");
                else if(s->row_count) {s->detail=s->rows[s->selected];s->details=true;}
            }
        }
        if(running) {collect(s);draw(s);}
    }
    esp_err_t stop_error;
    while((stop_error=poom_wifi_observer_stop(&s->observer))!=ESP_OK) {
        poom_arduboy_clear();line(0,"RADIO STOP FAILED");line(19,esp_err_to_name(stop_error));line(37,"RETRYING TEARDOWN");poom_arduboy_display();
        vTaskDelay(pdMS_TO_TICKS(250U));
    }
    portENTER_CRITICAL(&s_input_lock);s_input_queue=NULL;portEXIT_CRITICAL(&s_input_lock);
    if(s->subscribed) (void)poom_sbus_unsubscribe_cb("input/button",input,(void *)s_user);
    vQueueDelete(s->buttons);
    portENTER_CRITICAL(&s_input_lock);s_ui=NULL;if(s_stop_requested)hunt=false;portEXIT_CRITICAL(&s_input_lock);
    free(s);
    if(hunt) app_fox_hunt_wifi_ap(hunt_bssid,hunt_channel,hunt_name);else resume_menu();
    vTaskDelete(NULL);
}
static void show(bool recon) {
    if(menu_wifi_observer_is_active()) return;
    observer_ui_t *s=calloc(1,sizeof(*s));
    if(!s) {resume_menu();return;}
    portENTER_CRITICAL(&s_input_lock);s_ui=s;s_stop_requested=false;portEXIT_CRITICAL(&s_input_lock);
    s->recon=recon;s->buttons=xQueueCreate(12U,sizeof(uint8_t));
    if(s->buttons) {
        portENTER_CRITICAL(&s_input_lock);s_input_queue=s->buttons;portEXIT_CRITICAL(&s_input_lock);
        s->subscribed=poom_sbus_subscribe_cb("input/button",input,(void *)s_user);
        if(s->subscribed && xTaskCreate(task,"wifi_observer_ui",6144U,s,4U,NULL)==pdPASS) return;
    }
    portENTER_CRITICAL(&s_input_lock);s_input_queue=NULL;portEXIT_CRITICAL(&s_input_lock);
    if(s->subscribed) (void)poom_sbus_unsubscribe_cb("input/button",input,(void *)s_user);
    if(s->buttons) vQueueDelete(s->buttons);
    free(s);portENTER_CRITICAL(&s_input_lock);s_ui=NULL;portEXIT_CRITICAL(&s_input_lock);resume_menu();
}
void menu_wifi_guard_show(void) {show(false);}
void menu_wifi_recon_show(void) {show(true);}
