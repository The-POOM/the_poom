// SPDX-License-Identifier: MIT
#include "wardrive_connect.h"
#include <assert.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include "nvs.h"
#ifndef ESP_FAIL
#define ESP_FAIL -1
#endif
#include <unistd.h>
static void setup(const char *json) {
    assert(!mkdir(WD_ROOT "/config",0700)||errno==EEXIST);
    FILE *f=fopen(WD_ROOT "/config/wardrive-setup.json","w");assert(f);
    assert(fputs(json,f)>=0&&!fclose(f));
}
/* In-memory poom_nvs stand-in: two keys, string values only. */
static char nvs_name[140],nvs_token[140];
static bool has_name,has_token,nvs_fail;
static unsigned nvs_inits;
static char *slot(const char *key,bool **present) {
    if(!strcmp(key,"wd_api_name")) { *present=&has_name; return nvs_name; }
    assert(!strcmp(key,"wd_api_token")); *present=&has_token; return nvs_token;
}
esp_err_t poom_secrets_init(void) { nvs_inits++; return nvs_fail?ESP_FAIL:ESP_OK; }
esp_err_t poom_secrets_set_str(const char *key,const char *value) {
    bool *present; char *v=slot(key,&present); assert(strlen(key)<=15&&value[0]);
    snprintf(v,140,"%s",value); *present=true; return ESP_OK;
}
esp_err_t poom_secrets_get_str(const char *key,char *out,size_t *length) {
    bool *present; char *v=slot(key,&present);
    if(!*present) return ESP_ERR_NVS_NOT_FOUND;
    if(strlen(v)+1>*length) return ESP_ERR_NVS_INVALID_LENGTH;
    strcpy(out,v); *length=strlen(v)+1; return ESP_OK;
}
esp_err_t poom_secrets_erase_key(const char *key) {
    bool *present; char *v=slot(key,&present);
    if(!*present) return ESP_ERR_NVS_NOT_FOUND;
    memset(v,0,140); *present=false; return ESP_OK;
}
static bool parse(const char *s,wd_credentials_t *out) { return wd_credentials_parse(s,strlen(s),out); }
int main(void) {
    wd_credentials_t c={0};
    assert(parse("{\"wifi\":{\"ssid\":\"test\",\"password\":\"12345678\"},\"wigle\":{\"api_name\":\"synthetic\",\"api_token\":\"not-a-real-token\",\"auto_upload\":false}}",&c));
    assert(!c.auto_upload&&!strcmp(c.ssid,"test"));
    wd_credentials_t original=c;
    assert(!parse("{\"wifi\":{\"ssid\":\"test\",\"password\":\"short\"}}",&c));
    assert(!memcmp(&c,&original,sizeof(c)));
    assert(!parse("{\"wifi\":{\"ssid\":\"test\",\"ssid\":\"duplicate\"}}",&c));
    assert(!parse("{\"wifi\":{\"ssid\":7}}",&c));
    assert(!parse("{\"wigle\":{\"api_name\":\"x:y\",\"api_token\":\"token\"}}",&c));
    assert(!parse("{\"wigle\":{\"api_name\":\"x\",\"api_token\":\"token\",\"auto_upload\":\"true\"}}",&c));
    assert(!parse("{\"wifi\":{\"ssid\":\"test\"}} trailing",&c));
    assert(!parse("{}",&c));
    /* Real SD import preserves omitted groups; stand-alone parse stays pure. */
    c=original;c.auto_upload=true;
    setup("{\"wigle\":{\"api_name\":\"new-name\",\"api_token\":\"new-token\"}}");
    assert(wd_credentials_import(&c)&&!strcmp(c.ssid,original.ssid)&&!strcmp(c.password,original.password));
    assert(!strcmp(c.api_name,"new-name")&&!strcmp(c.api_token,"new-token")&&!c.auto_upload);
    setup("{\"wifi\":{\"ssid\":\"open-test\"}}");
    assert(wd_credentials_import(&c)&&!strcmp(c.ssid,"open-test")&&!c.password[0]&&!strcmp(c.api_name,"new-name"));
    wd_credentials_t before=c;
    setup("{\"wigle\":{\"api_name\":\"incomplete\"}}");assert(!wd_credentials_import(&c)&&!memcmp(&c,&before,sizeof(c)));
    setup("{\"wifi\":{\"ssid\":\"replacement\",\"password\":\"synthetic-password\"},\"wigle\":{\"api_name\":\"full-name\",\"api_token\":\"full-token\",\"auto_upload\":true}}");
    assert(wd_credentials_import(&c)&&c.auto_upload&&!strcmp(c.ssid,"replacement")&&!strcmp(c.api_name,"full-name"));
    assert(parse("{\"wigle\":{\"api_name\":\"standalone\",\"api_token\":\"synthetic\"}}",&c)&&!c.ssid[0]);
    assert(!unlink(WD_ROOT "/config/wardrive-setup.json"));
    /* Persistence: only API name/token reach NVS; WiFi never does. */
    strcpy(c.ssid,"not-persisted");strcpy(c.password,"not-persisted");
    assert(wd_credentials_save(&c)==ESP_OK&&has_name&&has_token);
    assert(!strcmp(nvs_name,"standalone")&&!strcmp(nvs_token,"synthetic"));
    wd_credentials_t loaded;memset(&loaded,0x55,sizeof(loaded));loaded.ssid[0]='k';loaded.ssid[1]=0;
    assert(wd_credentials_load(&loaded)&&!strcmp(loaded.api_name,"standalone")&&!strcmp(loaded.api_token,"synthetic"));
    assert(!strcmp(loaded.ssid,"k")); /* chosen WiFi untouched */
    /* Deleting the SD setup file has no effect on stored keys. */
    setup("{\"wigle\":{\"api_name\":\"sd\",\"api_token\":\"sd\"}}");assert(!unlink(WD_ROOT "/config/wardrive-setup.json"));
    assert(wd_credentials_load(&loaded)&&!strcmp(loaded.api_token,"synthetic"));
    /* Partial: a blank token erases only the token. */
    wd_credentials_t partial={0};strcpy(partial.api_name,"only-name");
    assert(wd_credentials_save(&partial)==ESP_OK&&has_name&&!has_token);
    assert(!wd_credentials_load(&loaded)&&!strcmp(loaded.api_name,"only-name")&&!loaded.api_token[0]);
    strcpy(partial.api_name,"x:y");assert(wd_credentials_save(&partial)==ESP_ERR_INVALID_ARG&&!strcmp(nvs_name,"only-name"));
    wd_credentials_forget();assert(!has_name&&!has_token);
    assert(!wd_credentials_load(&loaded)&&!loaded.api_name[0]&&!loaded.api_token[0]);
    nvs_fail=true;assert(wd_credentials_save(&c)!=ESP_OK&&!has_name);assert(!wd_credentials_load(&loaded));nvs_fail=false;
    wd_credentials_clear(&c); for(size_t i=0;i<sizeof(c);i++) assert(!((unsigned char *)&c)[i]);
    puts("Credential validation, API-only SD import preserving WiFi and NVS API-key persistence tests passed");
}
