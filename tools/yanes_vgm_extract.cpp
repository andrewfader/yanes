#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>

static uint32_t u32(const std::vector<uint8_t>& b,size_t p){return static_cast<uint32_t>(b[p]|(b[p+1]<<8U)|(b[p+2]<<16U)|(b[p+3]<<24U));}
int main(int argc,char**argv){
  if(argc!=3){std::cerr<<"usage: yanes-vgm-extract input.vgm output.reg\n";return 2;}
  std::ifstream in(argv[1],std::ios::binary);std::vector<uint8_t>b((std::istreambuf_iterator<char>(in)),{});
  if(b.size()<0x40||b[0]!='V'||b[1]!='g'||b[2]!='m'||b[3]!=' '){std::cerr<<"not an uncompressed VGM file\n";return 1;}
  size_t p=0x40;const uint32_t rel=u32(b,0x34);if(rel)p=0x34U+rel;
  uint64_t waits=0,writes=0;std::ofstream out(argv[2]);out<<"# Extracted YM2612 writes; timestamps are native 7670454/144 Hz samples.\n";
  auto need=[&](size_t n){return p+n<=b.size();};
  while(p<b.size()){
    const uint8_t c=b[p++];
    if(c==0x66)break;
    if(c==0x52||c==0x53){if(!need(2))return 1;const uint8_t reg=b[p++],val=b[p++];const uint64_t native=waits*7670454ULL/(144ULL*44100ULL);out<<native<<' '<<std::hex<<std::setw(3)<<std::setfill('0')<<(unsigned(reg)+(c==0x53?0x100U:0U))<<' '<<std::setw(2)<<unsigned(val)<<std::dec<<'\n';++writes;continue;}
    if(c==0x61){if(!need(2))return 1;waits+=static_cast<uint16_t>(b[p]|(b[p+1]<<8U));p+=2;continue;}
    if(c==0x62){waits+=735;continue;}if(c==0x63){waits+=882;continue;}if(c>=0x70&&c<=0x7f){waits+=(c&15U)+1U;continue;}
    if(c>=0x80&&c<=0x8f){waits+=c&15U;continue;}
    if(c==0x67){if(!need(6)||b[p++]!=0x66)return 1;++p;const uint32_t n=u32(b,p);p+=4;if(!need(n))return 1;p+=n;continue;}
    if(c==0x4f||c==0x50){if(!need(1))return 1;++p;continue;}
    if(c>=0x51&&c<=0x5f){if(!need(2))return 1;p+=2;continue;}
    if(c==0xe0){if(!need(4))return 1;p+=4;continue;}
    if(c==0x90||c==0x91){if(!need(4))return 1;p+=4;continue;}if(c==0x92){if(!need(5))return 1;p+=5;continue;}
    if(c==0x93){if(!need(10))return 1;p+=10;continue;}if(c==0x94){if(!need(1))return 1;++p;continue;}if(c==0x95){if(!need(4))return 1;p+=4;continue;}
    std::cerr<<"unsupported VGM command 0x"<<std::hex<<unsigned(c)<<" at 0x"<<p-1<<'\n';return 1;
  }
  if(!writes){std::cerr<<"no YM2612 register writes found\n";return 1;}std::cout<<"extracted "<<writes<<" writes across "<<waits<<" VGM samples\n";return out?0:1;
}
