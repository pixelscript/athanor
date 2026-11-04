#include <cstdio>
#include <cmath>
#include <vector>
#include "age.h"
using namespace athanor;
static float mem[2*Age::kWowFloats];
int main(int argc,char**argv){ float a=atof(argv[1]), m=atof(argv[2]);
  Age ag; ag.Init(48000,mem); const int B=64; float l[B],r[B]; long n=0; std::vector<float> o;
  for(long b=0;b<48000*6/B;b++){ ag.SetParams(-1,a,m); for(int i=0;i<B;i++){ l[i]=r[i]=0.3f*sinf(2*M_PI*440*n/48000.0); n++;} ag.Process(l,r,B); for(int i=0;i<B;i++) o.push_back(l[i]); }
  // instantaneous freq via zero crossings after 1s
  std::vector<double> zc; for(size_t i=48000;i<o.size();i++) if(o[i-1]<0&&o[i]>=0){ double f=o[i-1]/(o[i-1]-o[i]); zc.push_back(i-1+f);}
  double mn=1e9,mx=0; for(size_t k=10;k<zc.size();k++){ double f=48000/(zc[k]-zc[k-1]); mn=fmin(mn,f); mx=fmax(mx,f);} 
  printf("a=%.1f m=%.1f freq range %.1f..%.1f Hz => +-%.0f cents\n",a,m,mn,mx,1200*log2(mx/mn)/2);
}
