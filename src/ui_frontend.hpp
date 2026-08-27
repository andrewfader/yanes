// Platform-neutral editor drawing and hit-testing. Included inside the YANES anonymous
// namespace so it can see Plugin, parameters, and the DSP helpers.
#pragma once

#include "ui_canvas.hpp"
#include "ui_layout.hpp"

constexpr int kGuiRows = 16;

bool hardware_fm_waveform(int waveform) {
  return waveform==17||(waveform>=27&&waveform<=30)||waveform==21||waveform==31||
         waveform==32||waveform==33||waveform==36;
}
bool gui_param_relevant(clap_id id,int waveform) {
  switch(id){
    case kDuty:return waveform==0||waveform==3||waveform==10||waveform==18||waveform==38||waveform==39;
    case kNoisePeriod:return waveform==2||waveform==18;
    case kNoiseMode:return waveform==2||waveform==12||waveform==14||waveform==16||waveform==18||waveform==19||
                           waveform==20||waveform==21||waveform==23||waveform==24||waveform==25||waveform==34||waveform==42;
    case kExpansionShape:return waveform==3||waveform==4||waveform==5||waveform==6||waveform==24||waveform==25||
                                waveform==26||waveform==34||waveform==35||waveform==38||waveform==39||waveform==40||
                                waveform==41||waveform==44||waveform==45||waveform==57;
    case kFmRatio:case kFmIndex:return waveform==7||waveform==49||waveform==51||waveform==55;
    case kHardwareEnvelope:case kEnvelopeRate:return true;
    case kDpcmRate:return waveform==9||waveform==18||waveform==32||waveform==33;
    case kGenesisAlgorithm:case kGenesisFeedback:return waveform==49||hardware_fm_waveform(waveform);
    case kChipCutoff:case kChipResonance:return waveform==38||waveform==39||waveform==52||waveform==53||waveform==56;
    case kWavetablePosition:return waveform==46||waveform==47||waveform==48||waveform==50||waveform==54;
    case kWavetableWarp:return waveform==46||waveform==47;
    case kAdditiveTilt:return waveform==48;
    case kFmBrightness:return waveform==7||waveform==49||waveform==51||waveform==55||hardware_fm_waveform(waveform);
    default:return true;
  }
}
const char* gui_param_name(clap_id id,int waveform){
  if(id!=kExpansionShape)return kSpecs[static_cast<size_t>(id)].name;
  if(waveform==57)return "Drum character";
  if(waveform==46||waveform==47||waveform==48||waveform==50||waveform==54)return "Table shape";
  if(waveform==7||waveform==49||waveform==51||waveform==55)return "FM character";
  return "Chip shape";
}
const char* gui_help(clap_id id) {
  switch(id) {
    case kWaveform:return "Selects the chip, oscillator, or MIDI-channel stack used to make sound.";
    case kDuty:return "Changes pulse width; narrower duties sound thinner and brighter.";
    case kNoisePeriod:return "Selects a hardware noise-clock period instead of a continuously tuned pitch.";
    case kNoiseMode:return "Switches the selected chip's alternate short, narrow, or white-noise behavior.";
    case kAttackMs:return "Sets how quickly a new note reaches full level.";
    case kReleaseMs:return "Sets how long a note fades after release.";
    case kExpansionShape:return "Changes the selected chip model's duty, wavetable, or distortion variant.";
    case kFmRatio:return "Sets the modulator frequency relative to the played note.";
    case kFmIndex:return "Controls FM modulation strength and harmonic complexity.";
    case kClockMode:return "Switches between NTSC and PAL timing, changing authentic pitch quantization.";
    case kRetroAmount:return "Blends in the console/television degradation section.";
    case kBitDepth:return "Reduces amplitude resolution for stepped digital grit.";
    case kOutputRate:return "Reduces effective sample rate for brighter or rougher aliasing.";
    case kChipCutoff:return "Sets the cutoff of chip-specific filtering, especially SID modes.";
    case kChipResonance:return "Emphasizes frequencies around the chip filter cutoff.";
    case kWavetablePosition:return "Morphs across sine, triangle, saw, and pulse regions.";
    case kWavetableWarp:return "Bends wavetable phase to reshape the harmonic balance.";
    case kFmBrightness:return "Changes carrier level and the perceived brightness of FM voices.";
    case kLayerMode:return "Adds a tuned or noise-based companion oscillator to every voice.";
    case kLayerMix:return "Balances the added layer against the primary oscillator.";
    case kTempoSync:return "Locks the arpeggiator and echo timing to host tempo.";
    case kStrictHardware:return "Hardware-like stack retriggering, and raw (non-bandlimited) NES pulses into the mixer.";
    case kSequenceLength:return "Sets how many user pitch steps play before the sequence repeats.";
    case kDpcmBaseKey:return "Maps this MIDI note to sample slot 1; following notes select following slots.";
    case kDpcmLoopMask:return "Stores which of the sixteen DPCM slots repeat after reaching trim end.";
    case kDpcmInitialLevel:return "Sets the NES seven-bit DAC level before the first DPCM bit is decoded.";
    case kDpcmTrimStart:return "Moves the shared non-destructive start boundary for DPCM slots.";
    case kDpcmTrimEnd:return "Moves the shared non-destructive end boundary for DPCM slots.";
    case kStackMuteMask:return "Stores muted MIDI channels; use the channel tiles above for easier editing.";
    case kStackSoloMask:return "Stores soloed MIDI channels; use the channel tiles above for easier editing.";
    case kPreset:return "Loads a complete starting recipe; subsequent edits remain fully automatable.";
    default:break;
  }
  if(id>=kSequence1&&id<=kSequence8)return "Sets this sequence step's pitch offset in semitones.";
  if(id>=kFmAttack&&id<=kFmRelease)return "Shapes the hardware FM operators' amplitude envelope.";
  if(id>=kFmDetune&&id<=kFmPmDepth)return "Programs the corresponding hardware FM operator or LFO register.";
  if(id>=kDrive&&id<=kChorusDepth)return "Shapes the internal drive, echo, and chorus effects rack.";
  return "Adjusts this part of the current sound; changes are immediately audible and automatable.";
}

void gui_request_flush(Plugin* p) {
  if (p->host) if (const auto* hp = static_cast<const clap_host_params_t*>(
          p->host->get_extension(p->host, CLAP_EXT_PARAMS)))
    hp->request_flush(p->host);
}

void gui_edit(Plugin* p, clap_id id, double value) {
  set_param(p, id, value);
  gui_push(p, Plugin::kGuiValue, id, p->params[id].load(std::memory_order_relaxed));
  gui_request_flush(p);
}

void gui_gesture_begin(Plugin* p, clap_id id, double value) {
  set_param(p, id, value);
  gui_push(p, Plugin::kGuiBegin, id, 0);
  gui_push(p, Plugin::kGuiValue, id, p->params[id].load(std::memory_order_relaxed));
  gui_request_flush(p);
}

void gui_gesture_end(Plugin* p, clap_id id) {
  gui_push(p, Plugin::kGuiEnd, id, 0);
  gui_request_flush(p);
}

void gui_click_set(Plugin* p, clap_id id, double value) {
  gui_gesture_begin(p, id, value);
  gui_gesture_end(p, id);
}

bool gui_choose_sample(Plugin* p, int slot);

void gui_mark_state(Plugin* p) {
  if (p->host) if (const auto* hs = static_cast<const clap_host_state_t*>(
          p->host->get_extension(p->host, CLAP_EXT_STATE)))
    hs->mark_dirty(p->host);
}

void gui_draw(Plugin* p, yanes::ui::Canvas& canvas) {
  using yanes::ui::Point;
  constexpr uint32_t bg=0x0b1119,panel=0x121c28,panel_hi=0x182638,border=0x26384b;
  constexpr uint32_t text=0xe8eef6,muted=0x8495a8,cyan=0x5bd8ff,green=0x42d392,amber=0xffc857,red=0xf0647d;
  canvas.clear(bg);
  canvas.draw_text(32,44,"YANES",text,230);
  canvas.draw_text(166,44,"RETRO CHIP WORKSTATION",cyan,620);
  const float peak_l=p->output_peak_l.load(std::memory_order_relaxed),peak_r=p->output_peak_r.load(std::memory_order_relaxed);
  const bool clipped=p->output_clipped.load(std::memory_order_relaxed);
  canvas.draw_text(1080,44,"OUT",muted,45);
  canvas.fill_rect(1125,22,180,8,border);
  canvas.fill_rect(1125,36,180,8,border);
  canvas.fill_rect(1125,22,static_cast<int>(180*std::clamp(peak_l,0.0f,1.0f)),8,clipped?red:green);
  canvas.fill_rect(1125,36,static_cast<int>(180*std::clamp(peak_r,0.0f,1.0f)),8,clipped?red:green);
  canvas.draw_text(1320,44,clipped?"CLIP":"16-VOICE • CLAP",clipped?red:muted,245);
  const char* tabs[] = {"01  CHIP", "02  HARDWARE", "03  SYNTH", "04  SEQUENCE", "05  FM + BANK"};
  for (int i = 0; i < 5; ++i) {
    const bool active=i==p->gui_page,hover=i==p->gui_hover_tab;
    canvas.fill_rect(yanes::ui::tab_x+i*yanes::ui::tab_width,yanes::ui::tab_y,yanes::ui::tab_width-8,yanes::ui::tab_height-5,
                     active?panel_hi:(hover?0x162331:panel));
    if(active) canvas.fill_rect(yanes::ui::tab_x+i*yanes::ui::tab_width,yanes::ui::tab_y+yanes::ui::tab_height-8,yanes::ui::tab_width-8,3,cyan);
    canvas.draw_text(yanes::ui::tab_x+20+i*yanes::ui::tab_width,yanes::ui::tab_y+36,tabs[i],active?text:(hover?cyan:muted),yanes::ui::tab_width-40);
  }
  const char* page_titles[]={"CHIP VOICE","HARDWARE CHANNELS","WAVEFORM LAB","PITCH SEQUENCER","FM ROUTING + DPCM BANK"};
  const char* page_help[]={"Choose and shape the primary sound source","Map MIDI channels and add authentic hardware constraints","Build original digital tones and layered textures","Create tempo-synced tracker-style pitch movement","Program FM character and manage one-bit samples"};
  canvas.fill_rect(yanes::ui::visual_x,yanes::ui::visual_y,yanes::ui::visual_width,yanes::ui::visual_height,panel);
  canvas.draw_text(52,177,page_titles[p->gui_page],text,380);
  canvas.draw_text(52,211,page_help[p->gui_page],muted,390);
  if (p->gui_page == 1) {
    const uint32_t mute=static_cast<uint32_t>(p->params[kStackMuteMask].load()),solo=static_cast<uint32_t>(p->params[kStackSoloMask].load());
    for(int i=0;i<16;++i){const uint32_t bit=1U<<i,x=static_cast<uint32_t>(yanes::ui::mixer_x+i*yanes::ui::mixer_cell);const bool is_solo=solo&bit,is_mute=mute&bit;
      canvas.fill_rect(static_cast<int>(x),155,48,43,is_solo?amber:(is_mute?red:green));
      char n[4]{};std::snprintf(n,sizeof(n),"%02d",i+1);
      canvas.draw_text(static_cast<int>(x)+8,187,n,bg,35);
      canvas.draw_text(static_cast<int>(x)+5,221,is_solo?"SOLO":(is_mute?"MUTE":"ON"),is_solo?amber:(is_mute?red:muted),48);}
  } else if (p->gui_page == 3) {
    const int length = static_cast<int>(p->params[kSequenceLength].load());
    for (int i = 0; i < 8; ++i) { const double pitch = p->params[static_cast<clap_id>(kSequence1 + i)].load();
      const int x=yanes::ui::sequence_x+i*yanes::ui::sequence_cell;
      canvas.fill_rect(x,153,96,70,i<length?0x203d3a:0x182330);
      const int center=188,y=static_cast<int>(center-pitch*1.15);
      canvas.fill_rect(x,std::min(center,y),96,std::max(3,std::abs(center-y)),i<length?green:border);
      char step[8]{};std::snprintf(step,sizeof(step),"%d",i+1);canvas.draw_text(x+7,181,step,i<length?text:muted,22);
      char amount[12]{};std::snprintf(amount,sizeof(amount),"%+.0f",pitch);canvas.draw_text(x+52,219,amount,i<length?green:muted,42); }
  } else if (p->gui_page == 4) {
    const int waveform=static_cast<int>(p->params[kWaveform].load());
    const int ops = waveform==49 ? 6 : 4;
    const int algorithm = static_cast<int>(p->params[kGenesisAlgorithm].load()) & (ops==6?31:7);
    for(int i=0;i<ops;++i){const int x=470+i*88;canvas.fill_circle(x,160,34,green);
      if(i<ops-1&&(algorithm&(1<<std::min(i,3)))==0)canvas.draw_line(x+34,177,x+88,177,green);
      char op[3]{};std::snprintf(op,sizeof(op),"%d",i+1);canvas.draw_text(x+10,188,op,bg,18);}
    canvas.draw_text(470,220,"OPERATORS",muted,260);canvas.draw_text(815,177,"SAMPLES",muted,150);
    const uint32_t loops=static_cast<uint32_t>(p->params[kDpcmLoopMask].load());
    for(int i=0;i<16;++i){const auto bank=p->dpcm_banks[i].load();const bool loaded=bank&&!bank->empty(),loop=loops&(1U<<i);
      canvas.fill_rect(yanes::ui::bank_x+i*yanes::ui::bank_cell,157,27,38,loop?green:(loaded?amber:border));
      char n[3]{};std::snprintf(n,sizeof(n),"%X",i);canvas.draw_text(yanes::ui::bank_x+i*yanes::ui::bank_cell+6,187,n,loaded?bg:muted,18);}
    canvas.draw_text(yanes::ui::bank_x,220,"amber loaded  •  green looping",muted,550);
  } else if (p->gui_page == 2) {
    canvas.draw_line(yanes::ui::slider_x,188,1450,188,border);
    Point points[128]{};
    for(int i=0;i<128;++i){const double phase=i/127.0;const int x=yanes::ui::slider_x+i*980/127;
      points[i]={x,static_cast<int>(188-yanes::morph_wavetable(phase,p->params[kWavetablePosition].load(),p->params[kWavetableWarp].load())*28)};}
    canvas.draw_polyline(points,128,cyan);
    canvas.draw_text(1100,168,"FILTER RESPONSE",muted,240);
    const double cutoff=p->params[kChipCutoff].load(),res=p->params[kChipResonance].load();
    Point response[96]{};
    for(int i=0;i<96;++i){const double hz=40.0*std::pow(400.0,i/95.0),ratio=hz/std::max(40.0,cutoff),gain=1.0/std::sqrt(1.0+std::pow(ratio,4.0))*std::max(0.25,1.0+res*1.4*std::exp(-std::pow(std::log(std::max(0.001,ratio))/0.28,2.0)));
      response[i]={1100+i*420/95,220-static_cast<int>(std::clamp(gain,0.0,2.0)*24.0)};}
    canvas.draw_polyline(response,96,amber);
  } else if(p->gui_page==0){
    std::array<float,256> snapshot{};const uint32_t write=p->scope_write.load(std::memory_order_acquire);
    for(size_t i=0;i<snapshot.size();++i)snapshot[i]=p->scope_samples[(write+static_cast<uint32_t>(i))&255U].load(std::memory_order_relaxed);
    canvas.draw_text(470,168,"OUTPUT SCOPE",muted,210);canvas.draw_text(1115,168,"SPECTRUM",muted,180);
    canvas.draw_line(470,198,1045,198,border);canvas.draw_line(1095,224,1538,224,border);
    Point scope[128]{};for(int i=0;i<128;++i){const float sample=(snapshot[static_cast<size_t>(i*2)]+snapshot[static_cast<size_t>(i*2+1)])*0.5f;
      scope[i]={470+i*575/127,198-static_cast<int>(std::clamp(sample,-1.0f,1.0f)*28.0f)};}
    canvas.draw_polyline(scope,128,cyan);
    constexpr double tau=6.2831853071795864769;
    for(int band=0;band<24;++band){const int bin=std::clamp(static_cast<int>(std::lround(std::pow(2.0,band/5.2))),1,112);double real=0.0,imag=0.0;
      for(int n=0;n<256;++n){const double window=0.5-0.5*std::cos(tau*n/255.0);const double angle=tau*bin*n/256.0;
        real+=snapshot[static_cast<size_t>(n)]*window*std::cos(angle);imag-=snapshot[static_cast<size_t>(n)]*window*std::sin(angle);}
      const double magnitude=std::sqrt(real*real+imag*imag)/64.0;const int h=std::clamp(static_cast<int>(std::log1p(magnitude*7.0)*22.0),1,52);
      canvas.fill_rect(1098+band*18,224-h,12,h,band<16?green:amber);}
    canvas.draw_text(52,248,"ENVELOPE",muted,130);
    const double attack=p->params[kAttackMs].load(),release=p->params[kReleaseMs].load();
    const int ax=52+static_cast<int>(std::clamp(attack/500.0,0.0,1.0)*105.0),rx=270+static_cast<int>(std::clamp(release/2000.0,0.0,1.0)*110.0);
    canvas.draw_line(52,294,ax,258,green);canvas.draw_line(ax,258,270,258,green);canvas.draw_line(270,258,rx,294,green);
  }
  const int first = p->gui_page * kGuiRows;
  const int waveform=static_cast<int>(p->params[kWaveform].load(std::memory_order_relaxed));
  for (int row = 0; row < kGuiRows && first + row < static_cast<int>(kParamCount); ++row) {
    const int id = first + row, y = yanes::ui::rows_y + row * yanes::ui::row_height;
    const auto& s = kSpecs[static_cast<size_t>(id)];
    const double value = p->params[static_cast<size_t>(id)].load(std::memory_order_relaxed);
    const double norm = std::clamp((value - s.min) / (s.max - s.min),0.0,1.0);
    const bool hover=id==p->gui_hover_param,relevant=gui_param_relevant(static_cast<clap_id>(id),waveform);
    canvas.fill_rect(32,y,1536,yanes::ui::row_height-2,hover?panel_hi:((row&1)?0x0f1823:bg));
    canvas.draw_text(yanes::ui::label_x,y+31,gui_param_name(static_cast<clap_id>(id),waveform),relevant?(hover?text:0xcbd6e2):0x536273,yanes::ui::module_x-yanes::ui::label_x-14);
    canvas.draw_text(yanes::ui::module_x,y+31,s.module,relevant?(hover?cyan:muted):0x465464,yanes::ui::slider_x-yanes::ui::module_x-18);
    canvas.fill_rect(yanes::ui::slider_x,y+yanes::ui::rail_y_offset,yanes::ui::slider_width,yanes::ui::rail_height,border);
    const double default_norm=std::clamp((s.def-s.min)/(s.max-s.min),0.0,1.0);
    canvas.fill_rect(yanes::ui::slider_x+static_cast<int>(yanes::ui::slider_width*default_norm)-1,y+10,2,24,muted);
    canvas.fill_rect(yanes::ui::slider_x,y+yanes::ui::rail_y_offset,static_cast<int>(yanes::ui::slider_width*norm),yanes::ui::rail_height,hover?cyan:green);
    const int handle=yanes::ui::slider_x+static_cast<int>(yanes::ui::slider_width*norm);
    canvas.fill_rect(handle-4,y+9,8,26,text);
    char value_text[64]{}; value_to_text(nullptr, static_cast<clap_id>(id), value, value_text, sizeof(value_text));
    canvas.fill_rect(yanes::ui::value_x-12,y+6,370,32,hover?0x27384b:panel);
    if (s.stepped) {
      canvas.draw_text(yanes::ui::value_x,y+31,"‹",hover?amber:muted,24);
      canvas.draw_text(yanes::ui::value_x+30,y+31,value_text,hover?amber:text,276);
      canvas.draw_text(yanes::ui::value_x+322,y+31,"›",hover?amber:muted,20);
    } else canvas.draw_text(yanes::ui::value_x,y+31,value_text,hover?amber:text,340);
  }
  canvas.fill_rect(yanes::ui::tooltip_x,yanes::ui::tooltip_y,yanes::ui::tooltip_width,yanes::ui::tooltip_height,panel);
  if(p->gui_hover_param>=0&&p->gui_hover_param<static_cast<int>(kParamCount)){const auto id=static_cast<clap_id>(p->gui_hover_param);const auto&s=kSpecs[static_cast<size_t>(id)];char help[512]{};
    std::snprintf(help,sizeof(help),"%s  •  %s  —  %s%s  Drag/click set  •  wheel fine  •  right-click reset",gui_param_name(id,waveform),s.module,gui_help(id),gui_param_relevant(id,waveform)?"":"  (Inactive for this sound source.)");
    canvas.draw_text(52,1020,help,text,1490);}
  else {const char* footer=p->gui_page==1?"CHANNEL MIXER  —  left-click mute  •  right-click solo":(p->gui_page==4?"DPCM SLOTS  —  left-click loop  •  middle-click load  •  right-click clear":"Hover a control for help  •  every parameter supports host automation and modulation");
    canvas.draw_text(52,1020,footer,muted,1490);}
}

enum class GuiPointer { Move, Down, Up, Leave };

void gui_input(Plugin* p, GuiPointer action, int button, int x, int y) {
  if (action == GuiPointer::Leave) {
    if (p->gui_drag_param >= 0) { gui_gesture_end(p, static_cast<clap_id>(p->gui_drag_param)); p->gui_drag_param = -1; }
    p->gui_hover_param = -1;
    p->gui_hover_tab = -1;
    return;
  }
  if (action == GuiPointer::Move) {
    p->gui_hover_param = yanes::ui::param_row_at(p->gui_page, x, y, static_cast<int>(kParamCount));
    p->gui_hover_tab = yanes::ui::tab_at(x, y);
    if (p->gui_drag_param >= 0) {
      const auto& s = kSpecs[static_cast<size_t>(p->gui_drag_param)];
      gui_edit(p, static_cast<clap_id>(p->gui_drag_param), yanes::ui::value_from_x(x, s.min, s.max, s.stepped));
    }
    return;
  }
  if (action == GuiPointer::Up) {
    if (button == 1 && p->gui_drag_param >= 0) {
      gui_gesture_end(p, static_cast<clap_id>(p->gui_drag_param));
      p->gui_drag_param = -1;
    }
    return;
  }
  if (action != GuiPointer::Down) return;
  if (const int tab = yanes::ui::tab_at(x, y); tab >= 0) { p->gui_page = tab; return; }
  if (p->gui_page == 3) {
    const int step = yanes::ui::sequence_step_at(x, y);
    if (step >= 0) { gui_click_set(p, static_cast<clap_id>(kSequence1 + step), yanes::ui::sequence_pitch_at(y)); return; }
  }
  if (p->gui_page == 1) {
    const int channel = yanes::ui::mixer_channel_at(x, y);
    if (channel >= 0 && (button == 1 || button == 3)) {
      const clap_id id = button == 3 ? kStackSoloMask : kStackMuteMask;
      const uint32_t old = static_cast<uint32_t>(p->params[id].load());
      gui_click_set(p, id, static_cast<double>(old ^ (1U << channel)));
      return;
    }
  }
  if (p->gui_page == 4) {
    const int slot = yanes::ui::bank_slot_at(x, y);
    if (slot >= 0) {
      if (button == 2) { if (gui_choose_sample(p, slot)) gui_mark_state(p); return; }
      if (button == 3) { install_dpcm_bank(p, static_cast<size_t>(slot), {}); gui_mark_state(p); return; }
      if (button == 1) {
        const uint32_t old = static_cast<uint32_t>(p->params[kDpcmLoopMask].load());
        gui_click_set(p, kDpcmLoopMask, static_cast<double>(old ^ (1U << slot)));
        return;
      }
    }
  }
  if (const int row_id = yanes::ui::param_row_at(p->gui_page, x, y, static_cast<int>(kParamCount));
      row_id >= 0 && kSpecs[static_cast<size_t>(row_id)].stepped) {
    const int direction = yanes::ui::value_step_direction_at(x, y);
    if (direction >= 0 && (button == 1 || button == 4 || button == 5)) {
      const auto& s = kSpecs[static_cast<size_t>(row_id)];
      const double old = p->params[static_cast<size_t>(row_id)].load();
      const bool increase = button == 4 || (button == 1 && direction == 1);
      gui_click_set(p, static_cast<clap_id>(row_id), std::clamp(old + (increase ? 1.0 : -1.0), s.min, s.max));
      return;
    }
    if (direction >= 0 && button == 3) {
      gui_click_set(p, static_cast<clap_id>(row_id), kSpecs[static_cast<size_t>(row_id)].def);
      return;
    }
  }
  const int id = yanes::ui::param_at(p->gui_page, x, y, static_cast<int>(kParamCount));
  if (id < 0) return;
  const auto& s = kSpecs[static_cast<size_t>(id)];
  if (button == 1) {
    p->gui_drag_param = id;
    gui_gesture_begin(p, static_cast<clap_id>(id), yanes::ui::value_from_x(x, s.min, s.max, s.stepped));
  } else if (button == 4 || button == 5) {
    const double old = p->params[static_cast<size_t>(id)].load();
    const double step = s.stepped ? 1.0 : (s.max - s.min) / 100.0;
    gui_click_set(p, static_cast<clap_id>(id), std::clamp(old + (button == 4 ? step : -step), s.min, s.max));
  } else if (button == 3) {
    gui_click_set(p, static_cast<clap_id>(id), s.def);
  }
}
