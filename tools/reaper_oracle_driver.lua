-- Reaper-side driver for the Reaper+Furnace envelope oracle.
--
-- Builds a minimal single-voice project: track + YANES CLAP + a one-note MIDI
-- item. The Reaper offline render then exercises the full Reaper-bus, plugin
-- instantiation, MIDI event delivery, and track automation path — exactly the
-- code paths a real host goes through. The resulting WAV is the YANES render
-- with Reaper's host-side envelope shape. The corresponding Furnace WAV is
-- rendered separately and the two are compared by envelope_oracle.

local project_path = assert(os.getenv("YANES_REAPER_PROJECT"), "YANES_REAPER_PROJECT is required")
local marker_path   = assert(os.getenv("YANES_REAPER_MARKER"),   "YANES_REAPER_MARKER is required")
local render_dir    = assert(os.getenv("YANES_REAPER_RENDER_DIR"), "YANES_REAPER_RENDER_DIR is required")
local fixture_name  = assert(os.getenv("YANES_REAPER_FIXTURE"),  "YANES_REAPER_FIXTURE is required")
-- The note+duration window the oracle expects. Single-note fixtures are 0..3.2s.
local note_key      = tonumber(os.getenv("YANES_REAPER_NOTE_KEY"))    or 60
local note_velocity = tonumber(os.getenv("YANES_REAPER_NOTE_VEL"))    or 110
local note_off_s    = tonumber(os.getenv("YANES_REAPER_NOTE_OFF"))    or 1.6
local render_end_s  = tonumber(os.getenv("YANES_REAPER_RENDER_END"))  or 3.2

local waveform_index = tonumber(os.getenv("YANES_REAPER_WAVEFORM")) or 0  -- NES pulse default

local function mark(text)
    local file = assert(io.open(marker_path, "w"))
    file:write(text, "\n")
    file:close()
end

local function run()
    mark("started")
    reaper.InsertTrackAtIndex(0, true)
    local track = reaper.GetTrack(0, 0)
    reaper.GetSetMediaTrackInfo_String(track, "P_NAME", "YANES oracle", true)
    mark("track-created")

    local fx = reaper.TrackFX_AddByName(track, "CLAP: YANES", false, -1)
    if fx < 0 then fx = reaper.TrackFX_AddByName(track, "YANES", false, -1) end
    if fx < 0 then error("REAPER could not instantiate CLAP: YANES") end
    mark("plugin-instantiated")

    -- Select the voice with a stepped automation lane rather than a live
    -- parameter set. REAPER hands a live CLAP parameter change to the plug-in
    -- only when it next processes or flushes, which a headless session may never
    -- do before saving; this oracle once rendered the NES pulse for every fixture
    -- that way. Automation is part of the project, so the offline render delivers
    -- it to the plug-in as a timed parameter event. Points are in the units
    -- TrackFX_GetParam reports, which for a CLAP parameter is its native range
    -- (the lane is saved as PARMENV <id> 0 58): a normalized 0..1 point there
    -- would select waveform 0 for every fixture.
    local waveform_id = reaper.TrackFX_GetParamFromIdent(track, fx, ":0")
    if waveform_id < 0 then waveform_id = 0 end
    local _, lo, hi = reaper.TrackFX_GetParam(track, fx, waveform_id)
    if waveform_index < lo or waveform_index > hi then
        error(string.format("waveform %d outside the plug-in's range %g..%g", waveform_index, lo, hi))
    end
    reaper.TrackFX_SetParam(track, fx, waveform_id, waveform_index)
    local lane = reaper.GetFXEnvelope(track, fx, waveform_id, true)
    if not lane then error("REAPER could not create the Waveform automation lane") end
    reaper.InsertEnvelopePoint(lane, 0.0, waveform_index, 1, 0, false, true)
    reaper.InsertEnvelopePoint(lane, render_end_s, waveform_index, 1, 0, false, true)
    reaper.Envelope_SortPoints(lane)
    mark("voice-set")

    -- One-note item at t=0, like the Furnace module. REAPER delivers the t=0
    -- automation point at the head of the first block, ahead of the note; the
    -- timbre gate in the calling script proves the voice arrived in time.
    local item = reaper.CreateNewMIDIItemInProj(track, 0.0, render_end_s, false)
    local take = reaper.GetActiveTake(item)
    local start_ppq = reaper.MIDI_GetPPQPosFromProjTime(take, 0.0)
    local end_ppq   = reaper.MIDI_GetPPQPosFromProjTime(take, note_off_s)
    reaper.MIDI_InsertNote(take, false, false, start_ppq, end_ppq, 0, note_key, note_velocity, true)
    reaper.MIDI_Sort(take)
    mark("note-set")

    reaper.GetSetProjectInfo(0, "PROJECT_SRATE", 48000, true)
    reaper.GetSetProjectInfo(0, "RENDER_SRATE", 48000, true)
    reaper.GetSetProjectInfo(0, "RENDER_CHANNELS", 2, true)
    reaper.GetSetProjectInfo(0, "RENDER_BOUNDSFLAG", 0, true)
    reaper.GetSetProjectInfo(0, "RENDER_STARTPOS", 0.0, true)
    reaper.GetSetProjectInfo(0, "RENDER_ENDPOS", render_end_s, true)
    reaper.GetSetProjectInfo_String(0, "RENDER_FILE", render_dir, true)
    reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", fixture_name, true)
    reaper.GetSetProjectInfo_String(0, "RENDER_FORMAT", "evaw", true)
    reaper.Main_SaveProjectEx(0, project_path, 0)
    mark("project-saved")
    mark("ok")
end

local ok, err = xpcall(run, debug.traceback)
if not ok then
    local file = assert(io.open(marker_path, "w"))
    file:write("error: ", tostring(err), "\n")
    file:close()
end
reaper.defer(function() reaper.Main_OnCommand(40004, 0) end)
