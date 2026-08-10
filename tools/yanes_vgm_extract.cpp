#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

static uint32_t u32(const std::vector<uint8_t>& b,size_t p){return static_cast<uint32_t>(b[p]|(b[p+1]<<8U)|(b[p+2]<<16U)|(b[p+3]<<24U));}
int main(int argc,char**argv){
  if(argc!=4){std::cerr<<"usage: yanes-vgm-extract CHIP input.vgm output.reg\n"
                           "chips: ym2203 ym2608 ym2612 ym2151 ym3812 ymf262\n";return 2;}
  struct Chip { const char* name; uint8_t first,second; uint32_t clock,divider; };
  const Chip chips[]={{"ym2203",0x55,0,4000000,72},{"ym2608",0x56,0x57,8000000,144},
    {"ym2612",0x52,0x53,7670454,144},{"ym2151",0x54,0,3579545,64},
    {"ym3812",0x5a,0,3579545,72},{"ymf262",0x5e,0x5f,14318180,288}};
  const Chip* selected=nullptr;for(const auto& chip:chips)if(chip.name==std::string(argv[1]))selected=&chip;
  if(!selected){std::cerr<<"unsupported chip\n";return 2;}
  std::ifstream in(argv[2],std::ios::binary);std::vector<uint8_t>b((std::istreambuf_iterator<char>(in)),{});
  if(b.size()<0x40||b[0]!='V'||b[1]!='g'||b[2]!='m'||b[3]!=' '){std::cerr<<"not an uncompressed VGM file\n";return 1;}
  size_t p=0x40;const uint32_t rel=u32(b,0x34);if(rel)p=0x34U+rel;
  uint64_t waits=0,writes=0;std::ofstream out(argv[3]);out<<"# Extracted "<<selected->name<<" writes; timestamps are native chip samples.\n";
  auto need=[&](size_t n){return p+n<=b.size();};
  while(p<b.size()){
    const uint8_t c=b[p++];
    if(c==0x66)break;
    if(c>=0x51&&c<=0x5f){if(!need(2))return 1;const uint8_t reg=b[p++],val=b[p++];if(c==selected->first||c==selected->second){const uint64_t native=waits*selected->clock/(static_cast<uint64_t>(selected->divider)*44100ULL);out<<native<<' '<<std::hex<<std::setw(3)<<std::setfill('0')<<(unsigned(reg)+(c==selected->second?0x100U:0U))<<' '<<std::setw(2)<<unsigned(val)<<std::dec<<'\n';++writes;}continue;}
    if(c==0x61){if(!need(2))return 1;waits+=static_cast<uint16_t>(b[p]|(b[p+1]<<8U));p+=2;continue;}
    if(c==0x62){waits+=735;continue;}if(c==0x63){waits+=882;continue;}if(c>=0x70&&c<=0x7f){waits+=(c&15U)+1U;continue;}
    if(c>=0x80&&c<=0x8f){waits+=c&15U;continue;}
    if(c==0x67){if(!need(6)||b[p++]!=0x66)return 1;++p;const uint32_t n=u32(b,p);p+=4;if(!need(n))return 1;p+=n;continue;}
    if(c==0x4f||c==0x50){if(!need(1))return 1;++p;continue;}
    if(c==0xe0){if(!need(4))return 1;p+=4;continue;}
    if(c==0x90||c==0x91){if(!need(4))return 1;p+=4;continue;}if(c==0x92){if(!need(5))return 1;p+=5;continue;}
    if(c==0x93){if(!need(10))return 1;p+=10;continue;}if(c==0x94){if(!need(1))return 1;++p;continue;}if(c==0x95){if(!need(4))return 1;p+=4;continue;}
    std::cerr<<"unsupported VGM command 0x"<<std::hex<<unsigned(c)<<" at 0x"<<p-1<<'\n';return 1;
  }
  if(!writes){std::cerr<<"no "<<selected->name<<" register writes found\n";return 1;}std::cout<<"extracted "<<writes<<" writes across "<<waits<<" VGM samples\n";return out?0:1;
}
