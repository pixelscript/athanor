#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <vector>
#include "stretch_engine.h"
using namespace athanor;
static const float SR=48000;
static float Sig(long n){ double t=n/SR; float s=0; for(int h=1;h<=6;h++) s+=0.25f/h*sinf(2*M_PI*220*h*t); return s; }
int main(int argc,char**argv){
  float st=atof(argv[1]); int leash=atoi(argv[2]);
  StretchEngine e; e.Init(SR); e.SetGrainLeash(leash);
  const int B=64; float il[B],ir[B],ol[B],orr[B]; long n=0; std::vector<float> o;
  for(long b=0;b<SR*4/B;b++){ if(b==(long)(SR*1/B)){ e.SetStretch(st); e.SetSpeedLagSamples((st>1?st-1:0)*2.0*SR); e.Resync(); }
    for(int i=0;i<B;i++) il[i]=ir[i]=Sig(n++);
    e.Process(il,ir,ol,orr,B); for(int i=0;i<B;i++) o.push_back(ol[i]); }
  fwrite(o.data()+(long)(SR*1.5),4,(long)(SR*2.4),stdout);
}
