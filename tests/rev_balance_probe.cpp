#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <vector>
#include "fx.h"
using namespace athanor;
static float plate[Plate::kMemFloats], dl[Space::kDelayFloats], dr[Space::kDelayFloats];
int main(int argc,char**argv){ int side=atoi(argv[1]); float amt=atof(argv[2]), fb=atof(argv[3]);
  Space sp; sp.Init(48000,plate,dl,dr); sp.SetDelaySeconds(0.375f);
  const int B=64; float l[B],r[B]; long n=0; double sl=0,sr=0,pl=0,pr=0; int cl=0,cr=0; srand(1);
  for(long b=0;b<48000*10/B;b++){ sp.SetParams(side,amt,fb); for(int i=0;i<B;i++){ double t=n/48000.0; double ph=fmod(t,0.25); float s=(ph<0.03? 0.7f*((rand()/(float)RAND_MAX)*2-1)*(float)exp(-ph*150):0) + 0.25f*sinf(2*M_PI*55*t)*(float)(ph<0.12); l[i]=s; r[i]=s; n++;} sp.Process(l,r,B);
    for(int i=0;i<B;i++){ sl+=l[i]*l[i]; sr+=r[i]*r[i]; pl=fmax(pl,fabs(l[i])); pr=fmax(pr,fabs(r[i])); cl+=fabs(l[i])>0.8; cr+=fabs(r[i])>0.8; } }
  printf("side %d amt %.1f fb %.1f  L rms %.3f peak %.2f >0.8:%d   R rms %.3f peak %.2f >0.8:%d\n",side,amt,fb,sqrt(sl/n),pl,cl,sqrt(sr/n),pr,cr);
}
