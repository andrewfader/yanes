-- Lua script for REAPER to generate a full 5-track NES arrangement using YANES CLAP plugin
local project_path = os.getenv("YANES_REAPER_PROJECT") or "nes_mario_theme.rpp"
local marker_path = os.getenv("YANES_REAPER_MARKER")
local render_dir = os.getenv("YANES_REAPER_RENDER_DIR")

local function mark(text)
  if marker_path then
    local file = io.open(marker_path, "w")
    if file then
      file:write(text, "\n")
      file:close()
    end
  end
end

local function run()
  mark("started")

  -- Set BPM to 100 (quarter note = 0.6s, 16th note = 0.15s)
  reaper.SetCurrentBPM(0, 100.0, true)
  reaper.GetSetProjectInfo(0, "PROJECT_SRATE", 48000, true)
  reaper.GetSetProjectInfo(0, "RENDER_SRATE", 48000, true)
  reaper.GetSetProjectInfo(0, "RENDER_CHANNELS", 2, true)
  reaper.GetSetProjectInfo(0, "RENDER_BOUNDSFLAG", 0, true)
  reaper.GetSetProjectInfo(0, "RENDER_STARTPOS", 0.0, true)
  reaper.GetSetProjectInfo(0, "RENDER_ENDPOS", 8.0, true)

  if render_dir then
    reaper.GetSetProjectInfo_String(0, "RENDER_FILE", render_dir, true)
    reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", "yanes-nes-song", true)
    reaper.GetSetProjectInfo_String(0, "RENDER_FORMAT", "evaw", true)
  end

  local tracks_spec = {
    { name = "NES Pulse 1 (Lead)",     waveform = 0, duty = 2, pan = -0.2, vol = 0.9 }, -- 50% duty
    { name = "NES Pulse 2 (Harmony)",  waveform = 0, duty = 1, pan = 0.2,  vol = 0.8 }, -- 25% duty
    { name = "NES Triangle (Bass)",    waveform = 1, duty = 0, pan = 0.0,  vol = 1.0 },
    { name = "NES Noise (Percussion)", waveform = 2, duty = 0, pan = 0.0,  vol = 0.75 }
  }

  local track_handles = {}
  local takes = {}

  for i, spec in ipairs(tracks_spec) do
    reaper.InsertTrackAtIndex(i - 1, true)
    local tr = reaper.GetTrack(0, i - 1)
    track_handles[i] = tr
    reaper.GetSetMediaTrackInfo_String(tr, "P_NAME", spec.name, true)
    reaper.SetMediaTrackInfo_Value(tr, "D_PAN", spec.pan)
    reaper.SetMediaTrackInfo_Value(tr, "D_VOL", spec.vol)

    local fx = reaper.TrackFX_AddByName(tr, "CLAP: YANES", false, -1)
    if fx < 0 then fx = reaper.TrackFX_AddByName(tr, "YANES", false, -1) end
    if fx >= 0 then
      -- Param 0: Waveform, Param 1: Pulse duty, Param 4: Attack (0ms), Param 5: Release (0ms)
      local p_wave = reaper.TrackFX_GetParamFromIdent(tr, fx, ":0")
      if p_wave < 0 then p_wave = 0 end
      reaper.TrackFX_SetParamNormalized(tr, fx, p_wave, spec.waveform / 57.0)

      if spec.waveform == 0 then
        local p_duty = reaper.TrackFX_GetParamFromIdent(tr, fx, ":1")
        if p_duty < 0 then p_duty = 1 end
        reaper.TrackFX_SetParamNormalized(tr, fx, p_duty, spec.duty / 3.0)
      end

      -- Zero attack & zero release for punchy NES hardware envelope
      local p_att = reaper.TrackFX_GetParamFromIdent(tr, fx, ":4")
      if p_att >= 0 then reaper.TrackFX_SetParamNormalized(tr, fx, p_att, 0.0) end
      local p_rel = reaper.TrackFX_GetParamFromIdent(tr, fx, ":5")
      if p_rel >= 0 then reaper.TrackFX_SetParamNormalized(tr, fx, p_rel, 0.0) end
    end

    local item = reaper.CreateNewMIDIItemInProj(tr, 0.0, 8.0, false)
    takes[i] = reaper.GetActiveTake(item)
  end

  local function add_note(take, pitch, start_sec, dur_sec, vel)
    local start_ppq = reaper.MIDI_GetPPQPosFromProjTime(take, start_sec)
    local end_ppq = reaper.MIDI_GetPPQPosFromProjTime(take, start_sec + dur_sec)
    reaper.MIDI_InsertNote(take, false, false, start_ppq, end_ppq, 0, pitch, vel or 100, true)
  end

  -- Timing grid helper: sixteenth note = 0.15s at 100 BPM
  local step = 0.15

  -- Track 1: Pulse 1 Lead (Super Mario Bros Overworld)
  local t1 = takes[1]
  -- Intro: E5, E5, (rest), E5, C5, E5, G5, G4
  add_note(t1, 76, 0 * step, step * 0.85, 110) -- E5
  add_note(t1, 76, 1 * step, step * 0.85, 110) -- E5
  add_note(t1, 76, 3 * step, step * 0.85, 110) -- E5
  add_note(t1, 72, 5 * step, step * 0.85, 110) -- C5
  add_note(t1, 76, 6 * step, step * 0.85, 110) -- E5
  add_note(t1, 79, 8 * step, step * 1.85, 120) -- G5
  add_note(t1, 67, 12 * step, step * 1.85, 120) -- G4

  -- Main Theme Part A:
  -- C5, G4, E4, A4, B4, Bb4, A4, G4, E5, G5, A5, F5, G5, E5, C5, D5, B4
  local m_start = 16 * step
  add_note(t1, 72, m_start + 0 * step, step * 1.35, 110) -- C5
  add_note(t1, 67, m_start + 2 * step, step * 1.35, 100) -- G4
  add_note(t1, 64, m_start + 4 * step, step * 1.35, 100) -- E4
  add_note(t1, 69, m_start + 7 * step, step * 0.85, 110) -- A4
  add_note(t1, 71, m_start + 9 * step, step * 0.85, 110) -- B4
  add_note(t1, 70, m_start + 11 * step, step * 0.85, 110) -- Bb4
  add_note(t1, 69, m_start + 12 * step, step * 1.35, 110) -- A4

  add_note(t1, 67, m_start + 14 * step, step * 0.9, 105) -- G4
  add_note(t1, 76, m_start + 16 * step, step * 0.9, 115) -- E5
  add_note(t1, 79, m_start + 18 * step, step * 0.9, 115) -- G5
  add_note(t1, 81, m_start + 20 * step, step * 1.35, 120) -- A5
  add_note(t1, 77, m_start + 22 * step, step * 0.85, 110) -- F5
  add_note(t1, 79, m_start + 24 * step, step * 0.85, 110) -- G5

  add_note(t1, 76, m_start + 26 * step, step * 1.35, 110) -- E5
  add_note(t1, 72, m_start + 28 * step, step * 0.85, 100) -- C5
  add_note(t1, 74, m_start + 30 * step, step * 0.85, 100) -- D5
  add_note(t1, 71, m_start + 32 * step, step * 1.85, 100) -- B4

  -- Track 2: Pulse 2 Harmony
  local t2 = takes[2]
  -- Intro harmony: D5, D5, D5, B4, D5, G4, G3
  add_note(t2, 62, 0 * step, step * 0.85, 95)
  add_note(t2, 62, 1 * step, step * 0.85, 95)
  add_note(t2, 62, 3 * step, step * 0.85, 95)
  add_note(t2, 59, 5 * step, step * 0.85, 95)
  add_note(t2, 62, 6 * step, step * 0.85, 95)
  add_note(t2, 67, 8 * step, step * 1.85, 100)
  add_note(t2, 55, 12 * step, step * 1.85, 100)

  -- Main Theme Harmony:
  add_note(t2, 64, m_start + 0 * step, step * 1.35, 95)
  add_note(t2, 60, m_start + 2 * step, step * 1.35, 90)
  add_note(t2, 55, m_start + 4 * step, step * 1.35, 90)
  add_note(t2, 60, m_start + 7 * step, step * 0.85, 95)
  add_note(t2, 62, m_start + 9 * step, step * 0.85, 95)
  add_note(t2, 61, m_start + 11 * step, step * 0.85, 95)
  add_note(t2, 60, m_start + 12 * step, step * 1.35, 95)

  add_note(t2, 60, m_start + 14 * step, step * 0.9, 90)
  add_note(t2, 67, m_start + 16 * step, step * 0.9, 100)
  add_note(t2, 71, m_start + 18 * step, step * 0.9, 100)
  add_note(t2, 72, m_start + 20 * step, step * 1.35, 105)
  add_note(t2, 69, m_start + 22 * step, step * 0.85, 95)
  add_note(t2, 71, m_start + 24 * step, step * 0.85, 95)

  add_note(t2, 67, m_start + 26 * step, step * 1.35, 95)
  add_note(t2, 64, m_start + 28 * step, step * 0.85, 90)
  add_note(t2, 65, m_start + 30 * step, step * 0.85, 90)
  add_note(t2, 59, m_start + 32 * step, step * 1.85, 90)

  -- Track 3: Triangle Bassline
  local t3 = takes[3]
  -- Intro Bass: D3, D3, D3, D3, D3, G3, G2
  add_note(t3, 50, 0 * step, step * 0.85, 110)
  add_note(t3, 50, 1 * step, step * 0.85, 110)
  add_note(t3, 50, 3 * step, step * 0.85, 110)
  add_note(t3, 50, 5 * step, step * 0.85, 110)
  add_note(t3, 50, 6 * step, step * 0.85, 110)
  add_note(t3, 55, 8 * step, step * 1.85, 115)
  add_note(t3, 43, 12 * step, step * 1.85, 115)

  -- Main Bassline Groove (G3, E3, C3, A2, B2, Bb2, A2, G2...):
  add_note(t3, 55, m_start + 0 * step, step * 1.35, 110)
  add_note(t3, 52, m_start + 2 * step, step * 1.35, 110)
  add_note(t3, 48, m_start + 4 * step, step * 1.35, 110)
  add_note(t3, 45, m_start + 7 * step, step * 0.85, 110)
  add_note(t3, 47, m_start + 9 * step, step * 0.85, 110)
  add_note(t3, 46, m_start + 11 * step, step * 0.85, 110)
  add_note(t3, 45, m_start + 12 * step, step * 1.35, 110)

  add_note(t3, 43, m_start + 14 * step, step * 0.9, 110)
  add_note(t3, 52, m_start + 16 * step, step * 0.9, 110)
  add_note(t3, 55, m_start + 18 * step, step * 0.9, 110)
  add_note(t3, 57, m_start + 20 * step, step * 1.35, 115)
  add_note(t3, 53, m_start + 22 * step, step * 0.85, 110)
  add_note(t3, 55, m_start + 24 * step, step * 0.85, 110)

  add_note(t3, 52, m_start + 26 * step, step * 1.35, 110)
  add_note(t3, 48, m_start + 28 * step, step * 0.85, 110)
  add_note(t3, 50, m_start + 30 * step, step * 0.85, 110)
  add_note(t3, 47, m_start + 32 * step, step * 1.85, 110)

  -- Track 4: NES Noise Drum Pattern
  local t4 = takes[4]
  for s = 0, 52 do
    if s % 2 == 0 then
      local vel = (s % 4 == 2) and 110 or 75
      add_note(t4, 60, s * step, step * 0.45, vel)
    end
  end

  for i = 1, 4 do
    reaper.MIDI_Sort(takes[i])
  end

  reaper.Main_SaveProjectEx(0, project_path, 0)
  mark("ok")
end

local ok, err = xpcall(run, debug.traceback)
if not ok then
  mark("error: " .. tostring(err))
end
reaper.defer(function() reaper.Main_OnCommand(40004, 0) end)
