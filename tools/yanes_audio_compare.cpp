#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static uint16_t u16(const std::vector<uint8_t>& b, size_t p) { return static_cast<uint16_t>(b[p] | (b[p + 1] << 8U)); }
static uint32_t u32(const std::vector<uint8_t>& b, size_t p) { return static_cast<uint32_t>(u16(b,p) | (static_cast<uint32_t>(u16(b,p+2)) << 16U)); }
struct Wav { uint32_t rate{}; std::vector<double> samples; };
static Wav load(const char* path) {
  std::ifstream in(path, std::ios::binary); std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), {}); Wav w;
  if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data()+8, "WAVE", 4)) return w;
  uint16_t format=0,bits=0,channels=0; size_t at=0,n=0;
  for (size_t p=12;p+8<=b.size();) { const uint32_t z=u32(b,p+4); if(p+8U+z>b.size()) break;
    if(!std::memcmp(b.data()+p,"fmt ",4) && z>=16){format=u16(b,p+8);channels=u16(b,p+10);w.rate=u32(b,p+12);bits=u16(b,p+22);}
    if(!std::memcmp(b.data()+p,"data",4)){at=p+8;n=z;} p+=8U+z+(z&1U); }
  const size_t bytes=bits/8U;if((format!=1&&format!=3)||(format==3&&bits!=32)||(bits!=16&&bits!=24&&bits!=32)||!channels||!bytes||at+n>b.size())return {};
  const size_t frames=n/(bytes*channels);w.samples.reserve(frames);
  for(size_t frame=0;frame<frames;++frame){double mono=0;for(uint16_t channel=0;channel<channels;++channel){const size_t p=at+(frame*channels+channel)*bytes;double sample=0;if(format==3&&bits==32){float value{};std::memcpy(&value,b.data()+p,4);sample=value;}else if(bits==16)sample=static_cast<int16_t>(u16(b,p))/32768.0;else if(bits==24){int32_t value=static_cast<int32_t>(b[p]|(b[p+1]<<8U)|(b[p+2]<<16U));if(value&0x800000)value|=~0xffffff;sample=value/8388608.0;}else sample=static_cast<int32_t>(u32(b,p))/2147483648.0;if(!std::isfinite(sample))return {};mono+=sample;}w.samples.push_back(mono/channels);}return w;
}
int main(int argc,char**argv){
  if(argc<3||argc>4){std::cerr<<"usage: yanes-audio-compare reference.wav candidate.wav [minimum-correlation]\n";return 2;}
  Wav a=load(argv[1]),b=load(argv[2]); if(!a.rate||!b.rate){std::cerr<<"invalid WAV\n";return 1;}
  const size_t af=a.samples.size(),bf=b.samples.size();
  const size_t frames=std::min(af,static_cast<size_t>(bf*static_cast<double>(a.rate)/b.rate)); if(frames<64){std::cerr<<"not enough audio\n";return 1;}
  double aa=0,bb=0,ab=0,err=0;size_t n=0;std::vector<double> ea,eb;double blocka=0,blockb=0;size_t blockn=0;
  for(size_t f=0;f<frames;++f){const double pos=f*static_cast<double>(b.rate)/a.rate;const size_t q=std::min(bf-1,static_cast<size_t>(pos));const size_t q2=std::min(bf-1,q+1);const double frac=pos-q;
    const double x=a.samples[f],y=b.samples[q]*(1-frac)+b.samples[q2]*frac;aa+=x*x;bb+=y*y;ab+=x*y;blocka+=x*x;blockb+=y*y;++blockn;const double d=x-y;err+=d*d;++n;
    if((f&1023U)==1023U){ea.push_back(std::sqrt(blocka/blockn));eb.push_back(std::sqrt(blockb/blockn));blocka=blockb=0;blockn=0;}}
  if (aa < 1.0e-12 || bb < 1.0e-12) { std::cerr << "silent audio cannot validate a render\n"; return 1; }
  if (blockn) { ea.push_back(std::sqrt(blocka/blockn)); eb.push_back(std::sqrt(blockb/blockn)); }
  const double corr=ab/std::sqrt(std::max(1e-30,aa*bb)),rms=std::sqrt(err/n);
  double ma=0,mb=0;for(double x:ea)ma+=x;for(double x:eb)mb+=x;if(!ea.empty()){ma/=ea.size();mb/=eb.size();}
  double va=0,vb=0,cov=0;for(size_t i=0;i<ea.size();++i){const double x=ea[i]-ma,y=eb[i]-mb;va+=x*x;vb+=y*y;cov+=x*y;}const double envelope = va < 1e-20 && vb < 1e-20 ? 1.0 : cov/std::sqrt(std::max(1e-30,va*vb));
  std::cout<<"frames="<<frames<<" reference_rate="<<a.rate<<" candidate_rate="<<b.rate<<" waveform_correlation="<<corr<<" envelope_correlation="<<envelope<<" rms_error="<<rms<<'\n';
  double minimum = -1.0;
  if (argc == 4) {
    char* end = nullptr;
    minimum = std::strtod(argv[3], &end);
    if (end == argv[3] || *end || !std::isfinite(minimum) || minimum < -1.0 || minimum > 1.0) {
      std::cerr << "minimum correlation must be between -1 and 1\n"; return 2;
    }
  }
  return envelope>=minimum?0:1;
}
