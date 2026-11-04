#include <cstdio>
#include <cmath>
#include <vector>
#include "stretch_engine.h"
using namespace athanor;
static const float SR=48000;
int main(){
  StretchEngine e; e.Init(SR);
  const int B=64; float il[B],ir[B],ol[B],orr[B]; long n=0;
  float targets[]={1.0f,0.5f,2.0f,0.02f,1.0f};
  float p=1,v=0; const float w=2*M_PI*1.6f, z=0.5f;
  std::vector<float> o, pp;
  for(int seg=0;seg<5;seg++) for(long b=0;b<SR*1.5/B;b++){
    float dt=B/SR; v+=(w*w*(targets[seg]-p)-2*z*w*v)*dt; p+=v*dt; if(p<0.02f){p=0.02f; if(v<0)v=0;}
    e.SetPitch(p); pp.push_back(p);
    for(int i=0;i<B;i++){ double t=n/SR; il[i]=ir[i]=0.4f*sinf(2*M_PI*440*t) + ((n%12000)<200? 0.5f*sinf(2*M_PI*3000*t):0); n++; }
    e.Process(il,ir,ol,orr,B); for(int i=0;i<B;i++){ if(!std::isfinite(ol[i])){printf("NAN\n");return 1;} o.push_back(ol[i]); }
  }
  // dominant frequency at end of each segment
  for(int seg=0;seg<5;seg++){
    long s0=(long)((seg+1)*1.5*SR)-9600; double best=0,bf=0;
    for(double f=50;f<1500;f+=2){ double re=0,im=0; for(int k=0;k<9600;k++){ re+=o[s0+k]*cos(2*M_PI*f*k/SR); im+=o[s0+k]*sin(2*M_PI*f*k/SR);} double m=re*re+im*im; if(m>best){best=m;bf=f;} }
    double rms=0; for(int k=0;k<9600;k++) rms+=o[s0+k]*o[s0+k];
    printf("target %.2f -> reel %.3f  dominant %.0f Hz (expect %.0f)  rms %.3f\n",targets[seg],pp[(seg+1)*1125-1],bf,440*targets[seg],sqrt(rms/9600));
  }
  float mx=0; for(long i=1125;i<1125+400;i++) mx=fmax(mx,1-pp[i]); printf("overshoot on 1->0.5: reel min %.3f\n",1-mx);
}
