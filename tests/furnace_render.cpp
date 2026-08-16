#include "clap_harness.hpp"
#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
#include <cstdlib>

using namespace harness;

void put16(std::ofstream& out,uint16_t v){out.put(static_cast<char>(v));out.put(static_cast<char>(v>>8));}
void put32(std::ofstream& out,uint32_t v){put16(out,static_cast<uint16_t>(v));put16(out,static_cast<uint16_t>(v>>16));}

int main(int argc,char**argv){
  if(argc!=4)return 2;
  // Selecting the voice is all the setup a fixture is allowed to do: the chip's
  // register state comes from the plugin's own per-voice defaults, so this suite
  // measures what a user hears when they pick the voice and play a note. The only
  // other fields are how the reference module articulates the note — which key,
  // how long it is held, and the release the chip's own envelope leaves behind.
  struct Fixture{const char* name;double waveform;int channel,note_blocks,key;};
  constexpr Fixture fixtures[]={
    {"nes-pulse",0,0,153,72},{"nes-triangle",1,0,153,60},{"nes-noise",2,0,153,60},
    {"gameboy-pulse",10,0,44,60},{"gameboy-wave",11,0,153,48},{"gameboy-noise",12,0,41,60},
    {"sms-tone",13,0,153,84},{"sms-noise",14,0,153,84},{"pce-wave",26,0,153,72},
    {"ay-tone",22,0,153,60},{"pokey-tone",24,0,148,60},
    {"sid6581",38,0,1,60},{"sid8580",39,0,1,60},{"scc",40,0,147,72},
    {"saa1099",42,0,153,60},{"tia",44,0,153,60},{"vrc6-pulse",3,0,153,72},
    {"vrc6-saw",4,0,153,72},{"fds",5,0,153,72},{"n163",6,0,153,60},
    {"vrc7",7,0,165,60}};
  std::string requested=argv[2];int key_offset=0;
  if(requested.ends_with("-low")){requested.resize(requested.size()-4);key_offset=-12;}
  else if(requested.ends_with("-high")){requested.resize(requested.size()-5);key_offset=12;}
  const Fixture* selected=nullptr;for(const auto& fixture:fixtures)if(fixture.name==requested)selected=&fixture;
  if(!selected)return 2;
  const int note_blocks=selected->note_blocks;
  const Library library(argv[1]);const clap_plugin_t* plugin=library.create();
  std::vector<int16_t> pcm;pcm.reserve(48000U*3U*2U);
  {
    Runner runner(plugin,48000,512);runner.set(find_param(plugin,"Waveform"),selected->waveform);
    
    const int key=selected->key+key_offset;
    for(int block=0;block<300;++block){Events events;if(block==0)events.push(note_event(CLAP_EVENT_NOTE_ON,selected->channel,static_cast<int16_t>(key),1,1));if(block==note_blocks)events.push(note_event(CLAP_EVENT_NOTE_OFF,selected->channel,static_cast<int16_t>(key),1,0));runner.run((block==0||block==note_blocks)?&events:nullptr);for(uint32_t i=0;i<512;++i){const auto sample=[](float value){return static_cast<int16_t>(std::clamp(value,-1.0f,1.0f)*32767);};pcm.push_back(sample(runner.left()[i]));pcm.push_back(sample(runner.right()[i]));}}
  }
  plugin->destroy(plugin);std::ofstream out(argv[3],std::ios::binary);out.write("RIFF",4);put32(out,static_cast<uint32_t>(pcm.size()*2+36));out.write("WAVEfmt ",8);put32(out,16);put16(out,1);put16(out,2);put32(out,48000);put32(out,192000);put16(out,4);put16(out,16);out.write("data",4);put32(out,static_cast<uint32_t>(pcm.size()*2));out.write(reinterpret_cast<const char*>(pcm.data()),static_cast<std::streamsize>(pcm.size()*2));return out?0:1;
}
