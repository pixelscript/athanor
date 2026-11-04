#include <cstdio>
#include <cmath>
#include <vector>
#include "fx.h"
using namespace athanor;
static float plate[Plate::kMemFloats+4096], dl[Space::kDelayFloats], dr[Space::kDelayFloats];
int main(int argc,char**argv){ int side=atoi(argv[1]); float fb=atof(argv[2]);
  for(auto& x:plate) x=0;
  // guard region after arena filled with big values to expose overruns
  for(int i=Plate::kMemFloats;i<Plate::kMemFloats+4096;i++) plate[i]=100.0f;
  Space sp; sp.Init(48000,plate,dl,dr); sp.SetDelaySeconds(0.375f);
  for(int i=Plate::kMemFloats;i<Plate::kMemFloats+4096;i++) plate[i]=100.0f;
  const int B=64; float l[B],r[B]; long n=0; std::vector<float> L,R;
  for(long b=0;b<48000*6/B;b++){ sp.SetParams(side,0.8f,fb); for(int i=0;i<B;i++){ double t=n/48000.0; float s=0.3f*sinf(2*M_PI*330*t)*(fmod(t,1.0)<0.3); l[i]=s; r[i]=s; n++;} sp.Process(l,r,B); for(int i=0;i<B;i++){L.push_back(l[i]);R.push_back(r[i]);} }
  auto spikes=[&](std::vector<float>&x){ double m=0; int c=0; for(size_t i=2;i<x.size();i++){ double d2=fabs(x[i]-2*x[i-1]+x[i-2]); if(d2>0.05) c++; m=fmax(m,d2);} printf("maxd2 %.3f count>0.05: %d  ",m,c); };
  spikes(L); spikes(R); printf("\n");
}
