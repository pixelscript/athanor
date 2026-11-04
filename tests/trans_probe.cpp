#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <vector>
#include "stretch_engine.h"
using namespace athanor;
static const float SR=48000;
// clicks: 4 ms 2 kHz bursts every 0.5 s, each with a distinct pitch tail (pad) underneath
static float Sig(long n){ double t=n/SR; double ph=fmod(t,0.5); float c= ph<0.004? 0.8f*sinf(2*M_PI*2000*t)*(float)(1-ph/0.004):0;
  return c + 0.15f*sinf(2*M_PI*220*t); }
int main(int argc,char**argv){
  float st=atof(argv[1]); int leash=atoi(argv[2]); const char* out=argv[3];
  StretchEngine e; e.Init(SR); e.SetGrainLeash(leash);
  const int B=64; float il[B],ir[B],ol[B],orr[B]; long n=0; std::vector<float> o;
  for(long b=0;b<SR*6/B;b++){ if(b==(long)(SR*1/B)){ e.SetStretch(st); e.SetSpeedLagSamples((st>1?st-1:0)*2.0*SR); e.Resync(); }
    for(int i=0;i<B;i++) il[i]=ir[i]=Sig(n++);
    e.Process(il,ir,ol,orr,B); for(int i=0;i<B;i++) o.push_back(ol[i]); }
  FILE*f=fopen(out,"wb"); fwrite(o.data(),4,o.size(),f); fclose(f);
}
