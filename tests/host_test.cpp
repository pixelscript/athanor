// Host-side sanity test for Athanor DSP (no hardware).
#include <cstdio>
#include <cmath>
#include <vector>
#include <cstdlib>
#include "fx.h"
#include "drive.h"
#include "age.h"
#include "modfx.h"
#include "stretch_engine.h"
using namespace athanor;
static float plate[Plate::kMemFloats], dl[Space::kDelayFloats], dr[Space::kDelayFloats];
static const float SR=48000;
// drum-ish test signal: decaying noise bursts every 0.5 s + a tone
static float Sig(long n){ double t=n/SR; double ph=fmod(t,0.5); float burst=(float)exp(-ph*30)*((rand()/(float)RAND_MAX)*2-1)*0.6f;
  return burst + 0.2f*sinf(2*kPi*220*t)*(float)exp(-fmod(t,1.0)*2); }
struct Stats{double rms=0,peak=0;long n=0;bool bad=false; void add(float x){ if(!std::isfinite(x))bad=true; rms+=x*x; peak=fmax(peak,fabs(x)); n++;} void print(const char*s){printf("%-34s rms %.3f peak %.3f %s\n",s,sqrt(rms/n),peak,bad?"NAN!":"");}};
int main(){
  StretchEngine st; st.Init(SR); Drive dv; dv.Init(SR); static float am[2*Age::kWowFloats], cm[2*ChorusTrem::kFloats], pm[2*PhaserFlanger::kFloats]; Age ag; ag.Init(SR,am); static ChorusTrem ct; ct.Init(SR,cm); static PhaserFlanger pf; pf.Init(SR,pm); Space sp; sp.Init(SR,plate,dl,dr);
  const int B=64; float il[B],ir[B],ol[B],orr[B];
  long n=0;
  auto run=[&](const char* name,float seconds,auto setup,int stage){
    Stats s; Stats in;
    for(long b=0;b<(long)(seconds*SR/B);b++){
      setup();
      for(int i=0;i<B;i++){ il[i]=ir[i]=Sig(n++); in.add(il[i]); }
      if(stage==0){ st.Process(il,ir,ol,orr,B); }
      else { for(int i=0;i<B;i++){ol[i]=il[i];orr[i]=ir[i];} if(stage==1) dv.Process(ol,orr,B); else if(stage==2) sp.Process(ol,orr,B); else if(stage==3) ag.Process(ol,orr,B); else if(stage==4) ct.Process(ol,orr,B); else pf.Process(ol,orr,B); }
      for(int i=0;i<B;i++){ s.add(OutputLimit(ol[i])); s.add(OutputLimit(orr[i])); }
    }
    s.print(name);
  };
  printf("input reference: "); { Stats s; for(long i=0;i<SR*2;i++) s.add(Sig(i)); s.print(""); }
  run("stretch 1.0 (realtime)",3,[&]{st.SetStretch(1);},0);
  run("stretch 0.5 slow",4,[&]{st.SetStretch(0.5f);},0);
  run("stretch 0.0 freeze",3,[&]{st.SetStretch(0.0f);},0);
  st.Resync();
  run("stretch 2.0 speed (lag .5s)",4,[&]{st.SetGrainLeash(160); st.SetStretch(2); st.SetSpeedLagSamples(0.5*SR);},0);
  run("stretch 4.0 speed (lag 1.5s)",4,[&]{st.SetGrainLeash(40); st.SetStretch(4); st.SetSpeedLagSamples(1.5*SR);},0);
  run("stretch sweep 0..4",6,[&]{static float p=0; p+=0.0005f; float s=2+2*sinf(p); st.SetStretch(s); st.SetSpeedLagSamples((s>1?s-1:0)*0.5*SR);},0);
  printf("slices: %u\n", st.SliceCount());
  const char* L[3]={"sat","dist","fuzz"};
  for(int lv=0;lv<3;lv++) for(int sd=-1;sd<=1;sd+=2) for(float a: {0.3f,1.0f}){
    char nm[64]; snprintf(nm,64,"drive %s %s a=%.1f",L[lv],sd<0?"analog":"digital",a);
    run(nm,0.7f,[&]{dv.SetParams(lv,sd,a,0.5f);},1); }
  run("drive neutral",0.5f,[&]{dv.SetParams(2,0,0,0.5f);},1);
  run("age neutral",0.5f,[&]{ag.SetParams(0,0,0);},3);
  run("tape a=.5 m=0",1,[&]{ag.SetParams(-1,0.5f,0);},3);
  run("tape a=1 m=1",1,[&]{ag.SetParams(-1,1,1);},3);
  run("vinyl a=.5 m=0",1,[&]{ag.SetParams(1,0.5f,0);},3);
  run("vinyl a=1 m=1",1,[&]{ag.SetParams(1,1,1);},3);
  { Stats s; ag.SetParams(1,1,1); for(int b=0;b<SR/B;b++){ for(int i=0;i<B;i++)ol[i]=orr[i]=0; ag.Process(ol,orr,B); for(int i=0;i<B;i++) s.add(ol[i]); } s.print("vinyl noise floor (silence in)"); }
  run("age -> neutral",0.5f,[&]{ag.SetParams(0,0,0);},3);
  run("chorus a=.7 2Hz",1,[&]{ct.SetParams(-1,0.7f,{2.0f,0},0);},4);
  run("trem a=1 6Hz",1,[&]{ct.SetParams(1,1,{6.0f,0},0);},4);
  run("trem clocked 2/edge",1,[&]{static uint32_t e=0,k=0; if(++k%375==0) e++; ct.SetParams(1,0.8f,{4.0f,2.0f},e);},4);
  run("chorus/trem neutral",0.5f,[&]{ct.SetParams(0,0,{1,0},0);},4);
  run("phaser a=1 .5Hz",1,[&]{pf.SetParams(-1,1,{0.5f,0},0);},5);
  run("flanger a=1 .3Hz",1,[&]{pf.SetParams(1,1,{0.3f,0},0);},5);
  run("phaser/flanger neutral",0.5f,[&]{pf.SetParams(0,0,{1,0},0);},5);
  run("space neutral",1,[&]{sp.SetParams(0,0,0.5f);},2);
  run("space reverb 0.6 fb .5",3,[&]{sp.SetParams(-1,0.6f,0.5f);},2);
  run("space reverb 1.0 fb 1",6,[&]{sp.SetParams(-1,1,1);},2);
  run("space -> neutral tail",2,[&]{sp.SetParams(0,0,1);},2);
  run("space delay 0.6 fb .6",4,[&]{sp.SetDelaySeconds(0.25f); sp.SetParams(1,0.6f,0.6f);},2);
  run("space delay 1.0 fb 1.0",8,[&]{sp.SetDelaySeconds(0.3f); sp.SetParams(1,1,1);},2);
  run("space delay clock 3s(halved)",3,[&]{sp.SetDelaySeconds(3.0f); sp.SetParams(1,1,0.9f);},2);
  // silence after max feedback: does it blow up?
  { Stats s; sp.SetParams(1,1,1); for(int b=0;b<SR*10/B;b++){ for(int i=0;i<B;i++){ol[i]=orr[i]=0;} sp.Process(ol,orr,B); for(int i=0;i<B;i++) s.add(ol[i]); } s.print("space fb=1, 10s silence"); }
}
