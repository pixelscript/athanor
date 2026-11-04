#include <cstdio>
#include <cmath>
#include <vector>
#include "fx.h"
using namespace athanor;
static float mem[Plate::kMemFloats];
int main(){ Plate p; p.Init(48000,mem); p.SetDecay(0.85f); p.SetDamping(0.3f);
  std::vector<float> L,R; for(int i=0;i<48000*3;i++){ float l,r; p.Tick(i==0?1.0f:0.0f,l,r); L.push_back(l); R.push_back(r);} 
  double el=0,er=0; for(size_t i=0;i<L.size();i++){el+=L[i]*L[i]; er+=R[i]*R[i];}
  printf("energy L %.4f R %.4f ratio R/L %.2f dB\n",el,er,10*log10(er/el));
  // crest of early part: biggest single samples
  for(int w=0; w<6; w++){ int s=w*4800; double ml=0,mr=0,rl=0,rr=0; for(int i=s;i<s+4800;i++){ml=fmax(ml,fabs(L[i]));mr=fmax(mr,fabs(R[i])); rl+=L[i]*L[i]; rr+=R[i]*R[i];}
    printf("%3d-%3dms  L peak %.3f crest %.1f   R peak %.3f crest %.1f\n",w*100,w*100+100,ml,ml/sqrt(rl/4800+1e-12),mr,mr/sqrt(rr/4800+1e-12)); }
}
