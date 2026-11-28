// SPDX-License-Identifier: MIT
/* Independently implemented from documented SparkFun MT3333 wire behaviour.
 * No Arduino source is incorporated. See docs/WARDRIVE_GPS.md for references. */
#include "poom_gps.h"
#include "i2c.h"
static bool xa_open(void *ctx)
{
    (void)ctx;
    if(i2c_init()!=I2C_STATUS_OK) return false;
    i2c_lock();
    bool ok=true; /* Stream helper uses a temporary device handle. */
    /* MT3333 firmware stream initialisation: dummy zero write with STOP,
       followed by stream reads, NOT a u-blox register-address transaction. */
    uint8_t zero=0;
    if(ok) { ok=i2c_stream_transfer(0x10,&zero,1,NULL,0,30)==I2C_STATUS_OK; }
    i2c_unlock(); return ok;
}
static int xa_read(void *ctx,uint8_t *out,size_t capacity)
{
    (void)ctx;
    size_t n=capacity<32?capacity:32;
    i2c_lock();
    bool ok=i2c_stream_transfer(0x10,NULL,0,out,n,30)==I2C_STATUS_OK;
    i2c_unlock();
    if(!ok) return -1;
    /* The MT3333 stream uses LF as idle padding, including FIFO-empty reads
     * between portions of a sentence. SparkFun filters every LF, not just
     * padding after a completed line. Keep CR as the frame terminator and
     * leave generic NMEA LF handling unchanged for other transports. */
    size_t used=0;
    for(size_t i=0;i<n;i++) if(out[i]!=0x0a) out[used++]=out[i];
    return (int)used;
}
const poom_gps_adapter_t *poom_gps_xa1110(void)
{
    static const poom_gps_adapter_t a={"XA1110 / MT3333",xa_open,xa_read,NULL,NULL};
    return &a;
}
