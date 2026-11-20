// SPDX-License-Identifier: MIT
#include "CsiMotionModel.h"
#include "poom_csi_decode.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
static uint32_t rng=0x12345678;
static int8_t sample[260];
static void scalar(size_t at,int value){const uint16_t bits=(uint16_t)value & 0xfffU;sample[at]=(int8_t)bits;sample[at+1]=(int8_t)(bits>>8U);}
static void make_sample(bool moving){
    for(unsigned i=0;i<64;++i){
        rng=rng*1664525U+1013904223U;
        scalar(i*4,moving?(int)((rng>>16)%401)-200:80+(int)(i%7));
        rng=rng*1664525U+1013904223U;
        scalar(i*4+2,moving?(int)((rng>>16)%401)-200:20+(int)(i%5));
    }
}
static void feed(CsiMotionModel &m,uint32_t start,unsigned ms,bool moving,uint32_t &accepted){
    for(unsigned dt=0;dt<ms;dt+=10){make_sample(moving);CHECK(m.addSample(sample,256,-50,start+dt));++accepted;m.tick(start+dt,accepted,0);}
}
int main(){
    for(unsigned value=0;value<65536;++value){int8_t bytes[2]={(int8_t)value,(int8_t)(value>>8)};int expected=value&0xfffU;if(expected&0x800)expected-=4096;CHECK(poom_csi_decode12(bytes)==expected);}
    CsiMotionModel m;m.reset(0);CHECK(!m.addSample(nullptr,256,-50,0));
    std::memset(sample,0,sizeof(sample));for(size_t n=0;n<16;++n)CHECK(!m.addSample(sample,n,-50,0));
    CHECK(!m.addSample(sample,256,-50,0));m.tick(30000,0,0);CHECK(!m.calibrated());
    m.reset(0);uint32_t accepted=0;feed(m,0,1000,false,accepted);m.tick(31000,accepted,0);CHECK(!m.calibrated());
    m.reset(0);accepted=0;feed(m,0,35000,false,accepted);CHECK(m.calibrated());CHECK(m.motionScore()<CsiMotionModel::kPossibleMotionThreshold);CHECK(m.state()==CsiMotionModel::State::STILL);CHECK(m.samplesPerSecond()==100);CHECK(m.reliability()>=80);
    feed(m,35000,12000,true,accepted);CHECK(m.motionScore()>=CsiMotionModel::kMotionThreshold);CHECK(m.state()==CsiMotionModel::State::MOTION);
    m.tick(50000,accepted,0);CHECK(m.state()==CsiMotionModel::State::INSUFFICIENT_SAMPLES);CHECK(m.samplesPerSecond()==0);
    make_sample(false);CHECK(!m.addSample(sample,256,-50,100)); /* Late queued samples never run an unsigned-time loop. */
    m.tick(3600000,accepted,0);CHECK(m.state()==CsiMotionModel::State::INSUFFICIENT_SAMPLES);
    m.reset(UINT32_MAX-1000);accepted=0;feed(m,UINT32_MAX-1000,35000,false,accepted);CHECK(m.calibrated());CHECK(m.state()==CsiMotionModel::State::STILL);
    CsiMotionModel a,b;a.reset(0);b.reset(0);accepted=0;
    for(uint32_t now=0;now<35000;now+=20){
        make_sample(false);int8_t shifted[260];std::memcpy(shifted+4,sample,256);std::memset(shifted,0x7f,4);
        CHECK(a.addSample(shifted,260,-50,now,true));CHECK(b.addSample(sample,256,-50,now));
        ++accepted;a.tick(now,accepted,0);b.tick(now,accepted,0);
    }
    CHECK(a.motionScore()==b.motionScore());CHECK(a.reliability()==b.reliability());CHECK(a.state()==b.state());
    m.reset(0);accepted=0;for(unsigned i=0;i<70000;++i){make_sample(false);if(m.addSample(sample,256,-50,0))++accepted;}
    CHECK(accepted==65535U);m.tick(1000,accepted,0);CHECK(m.samplesPerSecond()==65535U);
    std::printf("CSI model: %u checks passed\n",checks);return 0;
}
