local project_path = assert(os.getenv("YANES_REAPER_PROJECT"), "YANES_REAPER_PROJECT is required")
local marker_path = assert(os.getenv("YANES_REAPER_MARKER"), "YANES_REAPER_MARKER is required")
local render_dir = assert(os.getenv("YANES_REAPER_RENDER_DIR"), "YANES_REAPER_RENDER_DIR is required")
local function mark(text)
  local file = assert(io.open(marker_path, "w"))
  file:write(text, "\n")
  file:close()
end

local function run()
mark("started")
reaper.InsertTrackAtIndex(0, true)
local track = reaper.GetTrack(0, 0)
reaper.GetSetMediaTrackInfo_String(track, "P_NAME", "YANES integration", true)
mark("track-created")

local fx = reaper.TrackFX_AddByName(track, "CLAP: YANES", false, -1)
if fx < 0 then fx = reaper.TrackFX_AddByName(track, "YANES", false, -1) end
if fx < 0 then
  local file = assert(io.open(marker_path, "w"))
  file:write("error: REAPER could not instantiate CLAP: YANES\n")
  file:close()
  return
end
mark("plugin-instantiated")

-- Exercise a non-default preset and sample-accurate host automation.
local preset = reaper.TrackFX_GetParamFromIdent(track, fx, ":78")
if preset < 0 then preset = 78 end
reaper.TrackFX_SetParamNormalized(track, fx, preset, 48.0 / 48.0)
mark("preset-set")
local waveform = reaper.TrackFX_GetParamFromIdent(track, fx, ":0")
if waveform < 0 then waveform = 0 end
local envelope = reaper.GetFXEnvelope(track, fx, waveform, true)
reaper.InsertEnvelopePoint(envelope, 0.0, 57.0 / 57.0, 0, 0, false, true)
reaper.InsertEnvelopePoint(envelope, 0.75, 51.0 / 57.0, 0, 0, false, true)
reaper.Envelope_SortPoints(envelope)
mark("automation-written")

local item = reaper.CreateNewMIDIItemInProj(track, 0.0, 2.0, false)
local take = reaper.GetActiveTake(item)
local function note(key, start_time, end_time, velocity)
  local start_ppq = reaper.MIDI_GetPPQPosFromProjTime(take, start_time)
  local end_ppq = reaper.MIDI_GetPPQPosFromProjTime(take, end_time)
  reaper.MIDI_InsertNote(take, false, false, start_ppq, end_ppq, 9, key, velocity, true)
end
for i = 0, 11 do note(36 + i, i * 0.055, i * 0.055 + 0.04, 72 + i * 4) end
note(60, 0.82, 1.65, 110)
reaper.MIDI_Sort(take)
mark("midi-written")

reaper.GetSetProjectInfo(0, "PROJECT_SRATE", 48000, true)
reaper.GetSetProjectInfo(0, "RENDER_SRATE", 48000, true)
reaper.GetSetProjectInfo(0, "RENDER_CHANNELS", 2, true)
reaper.GetSetProjectInfo(0, "RENDER_BOUNDSFLAG", 0, true)
reaper.GetSetProjectInfo(0, "RENDER_STARTPOS", 0.0, true)
reaper.GetSetProjectInfo(0, "RENDER_ENDPOS", 2.0, true)
reaper.GetSetProjectInfo_String(0, "RENDER_FILE", render_dir, true)
reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", "yanes-reaper", true)
reaper.GetSetProjectInfo_String(0, "RENDER_FORMAT", "evaw", true)
reaper.Main_SaveProjectEx(0, project_path, 0)
mark("project-saved")

mark("ok")
end

local ok, failure = xpcall(run, debug.traceback)
if not ok then
  local file = assert(io.open(marker_path, "w"))
  file:write("error: ", failure, "\n")
  file:close()
end
-- Startup scripts execute before REAPER enters its normal event loop. Defer
-- quit by one cycle so the command is honored after the project is flushed.
reaper.defer(function() reaper.Main_OnCommand(40004, 0) end)
