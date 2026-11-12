// SPDX-License-Identifier: MIT
#include "poom_gps.h"
#include "i2c.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static unsigned locks,unlocks,dummy,reads;
static bool connected=true,inject_padding=false;
static unsigned wire_bytes;
static const char *stream; static size_t stream_at;
uint8_t i2c_init(void) { return I2C_STATUS_OK; }
uint8_t i2c_register_device(uint8_t address) { assert(address==0x10); return I2C_STATUS_OK; }
void i2c_lock(void) { locks++; assert(locks==unlocks+1); }
void i2c_unlock(void) { unlocks++; assert(locks==unlocks); }
uint8_t i2c_stream_transfer(uint8_t address,const uint8_t *tx,size_t nt,uint8_t *rx,size_t nr,int timeout) {
    assert(locks==unlocks+1&&address==0x10&&timeout==30);
    if(!connected) return I2C_STATUS_ERROR;
    if(nt) { assert(nt==1&&tx[0]==0&&!nr&&!rx); dummy++; }
    else {
        assert(!tx&&nr<=32&&rx); reads++;
        for(size_t i=0;i<nr;i++) {
            /* Frequent padding while a sentence is unfinished, plus ordinary
             * CR/LF sentence endings and whole idle reads. */
            if(inject_padding&&++wire_bytes%3!=0) rx[i]=0x0a;
            else rx[i]=stream[stream_at]?(uint8_t)stream[stream_at++]:0x0a;
        }
    }
    return I2C_STATUS_OK;
}
int main(void) {
    char sentence[128]; const char *body="GNRMC,123519,A,4807.038,N,01131.000,E,,,071126,,,A";
    unsigned sum=0; for(const char *p=body;*p;p++) sum^=(unsigned char)*p;
    snprintf(sentence,sizeof(sentence),"$%s*%02X\r\n",body,sum); stream=sentence;
    poom_gps_service_t s; poom_gps_service_init(&s,poom_gps_xa1110());
    for(unsigned t=0;t<8;t++) poom_gps_service_poll(&s,t*40);
    assert(dummy==1&&reads==8&&s.parser.fix.fix&&s.parser.valid_sentences==1);
    assert(locks==unlocks);
    connected=false; poom_gps_service_poll(&s,400);
    assert(!s.opened&&!s.parser.fix.connected&&s.parser.fix.fields==0);
    connected=true; stream_at=0;
    for(unsigned t=2400;t<2800;t+=40) poom_gps_service_poll(&s,t);
    assert(dummy==2&&s.parser.fix.fix&&locks==unlocks);
    poom_gps_service_close(&s);
    /* Complete RMC survives LF padding at arbitrary boundaries, including
     * within a checksum; the old adapter would emit truncated fragments. */
    inject_padding=true; stream_at=0; wire_bytes=0;
    poom_gps_service_init(&s,poom_gps_xa1110());
    for(unsigned t=0;t<30;t++) poom_gps_service_poll(&s,3000+t*40);
    assert(s.parser.valid_sentences==1&&s.parser.fix.fix&&s.parser.length==0);
    assert(!s.parser.checksum_errors&&locks==unlocks);
    uint8_t out[32]; assert(poom_gps_xa1110()->read(NULL,out,sizeof(out))==0);
    poom_gps_service_close(&s);
    puts("Simulated XA1110 wire sequence, mid-sentence LF padding, idle reads and reconnect tests passed");
}
