// SPDX-License-Identifier: MIT
#include "wardrive_connect.h"
#include "cJSON.h"
#include "poom_secrets_store.h"
#include "nvs.h"
#include <string.h>
static bool text(cJSON *o,const char *key,char *out,size_t cap,bool required) {
    cJSON *x=cJSON_GetObjectItemCaseSensitive(o,key);
    if(!x) return !required;
    if(!cJSON_IsString(x)||strlen(x->valuestring)>=cap) return false;
    for(const unsigned char *s=(const unsigned char *)x->valuestring;*s;s++) if(*s<32||*s==127) return false;
    strcpy(out,x->valuestring); return !required||out[0];
}
static bool unique(cJSON *o) {
    if(!cJSON_IsObject(o)) return false;
    for(cJSON *x=o->child;x;x=x->next) for(cJSON *y=x->next;y;y=y->next) if(!strcmp(x->string,y->string)) return false;
    return true;
}
static bool parse(const char *json,size_t length,wd_credentials_t *out,bool preserve) {
    if(!json||!out||!length||length>2048||memchr(json,0,length)) return false;
    /* Bound cJSON recursion before parsing an untrusted SD file. Also reject
     * encoded NULs, which cJSON's C-string model cannot represent faithfully. */
    unsigned depth=0; bool quoted=false,escaped=false;
    for(size_t i=0;i<length;i++) {
        char ch=json[i];
        if(i+6<=length&&!memcmp(json+i,"\\u0000",6)) return false;
        if(quoted) {
            if(escaped) escaped=false;
            else if(ch=='\\') escaped=true;
            else if(ch=='"') quoted=false;
        } else if(ch=='"') quoted=true;
        else if(ch=='{'||ch=='[') { if(++depth>8) return false; }
        else if(ch=='}'||ch==']') { if(!depth) return false; depth--; }
    }
    if(depth||quoted) return false;
    const char *end=NULL;
    cJSON *root=cJSON_ParseWithLengthOpts(json,length,&end,false);
    if(!root) return false;
    while(end<json+length&&(*end==' '||*end=='\r'||*end=='\n'||*end=='\t')) end++;
    bool ok=end==json+length && unique(root);
    wd_credentials_t c=preserve?*out:(wd_credentials_t){0};
    c.auto_upload=false; /* imported groups never inherit a prior consent */
    cJSON *wifi=cJSON_GetObjectItemCaseSensitive(root,"wifi");
    cJSON *wigle=cJSON_GetObjectItemCaseSensitive(root,"wigle");
    if(wifi) {
        memset(c.ssid,0,sizeof(c.ssid)); memset(c.password,0,sizeof(c.password));
        ok=ok&&unique(wifi)&&text(wifi,"ssid",c.ssid,sizeof(c.ssid),true)&&text(wifi,"password",c.password,sizeof(c.password),false);
    }
    if(wigle) {
        ok=ok&&unique(wigle)&&text(wigle,"api_name",c.api_name,sizeof(c.api_name),true)&&text(wigle,"api_token",c.api_token,sizeof(c.api_token),true);
        cJSON *a=cJSON_GetObjectItemCaseSensitive(wigle,"auto_upload");
        if(a&&!cJSON_IsBool(a)) ok=false;
        c.auto_upload=cJSON_IsTrue(a);
        if(strchr(c.api_name,':')) ok=false;
    }
    if(!wifi&&!wigle) ok=false;
    if(c.password[0]&&strlen(c.password)<8) ok=false;
    if(strlen(c.password)==64) for(unsigned i=0;i<64;i++) if(!((c.password[i]>='0'&&c.password[i]<='9')||(c.password[i]>='a'&&c.password[i]<='f')||(c.password[i]>='A'&&c.password[i]<='F'))) ok=false;
    if(ok) *out=c;
    wd_credentials_clear(&c); cJSON_Delete(root); return ok;
}
bool wd_credentials_parse(const char *json,size_t length,wd_credentials_t *out) {
    return parse(json,length,out,false);
}
void wd_credentials_clear(wd_credentials_t *c) { volatile unsigned char *p=(volatile unsigned char *)c; for(size_t i=0;i<sizeof(*c);i++) p[i]=0; }
bool wd_credentials_import(wd_credentials_t *out) {
    FILE *f=fopen(WD_ROOT "/config/wardrive-setup.json","rb"); if(!f) return false;
    char data[2049]; size_t n=fread(data,1,sizeof(data),f); bool ok=!ferror(f)&&n<sizeof(data);
    fclose(f); if(ok) ok=parse(data,n,out,true);
    volatile char *p=data; for(size_t i=0;i<sizeof(data);i++) p[i]=0;
    return ok;
}
/* NVS keys (max 15 chars) in the shared poom_nvs secrets namespace. */
#define WD_KEY_API_NAME "wd_api_name"
#define WD_KEY_API_TOKEN "wd_api_token"
static esp_err_t store(const char *key,const char *value) {
    if(value[0]) return poom_secrets_set_str(key,value);
    esp_err_t e=poom_secrets_erase_key(key);
    return e==ESP_ERR_NVS_NOT_FOUND?ESP_OK:e;
}
esp_err_t wd_credentials_save(const wd_credentials_t *c) {
    /* poom_nvs is not encrypted at rest; the owner accepted the same
     * protection level as saved WiFi profiles (Nov 2026). Never logged. */
    if(!c||strchr(c->api_name,':')) return ESP_ERR_INVALID_ARG;
    esp_err_t e=poom_secrets_init();
    if(e!=ESP_OK) return e;
    e=store(WD_KEY_API_NAME,c->api_name);
    if(e==ESP_OK) e=store(WD_KEY_API_TOKEN,c->api_token);
    return e;
}
static bool fetch(const char *key,char *out,size_t capacity) {
    size_t length=capacity;
    if(poom_secrets_get_str(key,out,&length)!=ESP_OK||!memchr(out,0,capacity)) { memset(out,0,capacity); return false; }
    for(const unsigned char *p=(const unsigned char *)out;*p;p++) if(*p<32||*p==127) { memset(out,0,capacity); return false; }
    return out[0]!=0;
}
bool wd_credentials_load(wd_credentials_t *c) {
    memset(c->api_name,0,sizeof(c->api_name)); memset(c->api_token,0,sizeof(c->api_token));
    if(poom_secrets_init()!=ESP_OK) return false;
    bool name=fetch(WD_KEY_API_NAME,c->api_name,sizeof(c->api_name));
    bool token=fetch(WD_KEY_API_TOKEN,c->api_token,sizeof(c->api_token));
    if(strchr(c->api_name,':')) { memset(c->api_name,0,sizeof(c->api_name)); name=false; }
    return name&&token;
}
void wd_credentials_forget(void) {
    if(poom_secrets_init()!=ESP_OK) return;
    (void)poom_secrets_erase_key(WD_KEY_API_NAME);
    (void)poom_secrets_erase_key(WD_KEY_API_TOKEN);
}
