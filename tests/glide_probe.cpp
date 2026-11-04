// Render speed-up / slow-down to WAV-ish raw floats for inspection.
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <vector>
#include "stretch_engine.h"
using namespace athanor;
static const float SR=48000;
// clicks every 0.25s with pitch rising each beat (so time order is visible), plus continuous tone 440
static float Sig(long n){ double t=n/SR; return 0.4f*sinf(2*M_PI*(300*t+25*t*t)); }
int main(int argc,char**argv){
  float st=atof(argv[1]); int leash=atoi(argv[2]); double T=2.0; const char* out=argv[3];
  StretchEngine e; e.Init(SR); e.SetGrainLeash(leash);
  const int B=64; float il[B],ir[B],ol[B],orr[B]; long n=0; std::vector<float> o;
  for(long b=0;b<SR*8/B;b++){ if(b==(long)(SR*1/B)){ e.SetStretch(st); e.SetSpeedLagSamples((st>1?st-1:0)*T*SR); e.Resync(); }
    for(int i=0;i<B;i++) il[i]=ir[i]=Sig(n++);
    e.Process(il,ir,ol,orr,B); for(int i=0;i<B;i++) o.push_back(ol[i]); }
  FILE*f=fopen(out,"wb"); fwrite(o.data(),4,o.size(),f); fclose(f);
  double pk=0,rms=0; for(float x:o){pk=fmax(pk,fabs(x)); rms+=x*x;} printf("st %.2f leash %d rms %.3f peak %.3f\n",st,leash,sqrt(rms/o.size()),pk);
}
