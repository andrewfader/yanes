-- Reaper-side driver for the Reaper+Furnace envelope oracle.
--
-- Builds a minimal single-voice project: track + YANES CLAP + a one-note MIDI
-- item. The Reaper offline render then exercises the full Reaper-bus, plugin
-- instantiation, MIDI event delivery, and track automation path — exactly the
-- code paths a real host goes through. The resulting WAV is the YANES render
-- with Reaper's host-side envelope shape. The corresponding Furnace WAV is
-- rendered separately and the two are compared by envelope_furnace_oracle.

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
    if fx < 0 then
        local file = assert(io.open(marker_path, "w"))
        file:write("error: REAPER could not instantiate CLAP: YANES\n")
        file:close()
        return
    end
    mark("plugin-instantiated")

    -- Default fixture setting: pick the requested voice at full preset slot.
    local waveform_id = reaper.TrackFX_GetParamFromIdent(track, fx, ":0")
    if waveform_id < 0 then waveform_id = 0 end
    reaper.TrackFX_SetParamNormalized(track, fx, waveform_id, waveform_index / 57.0)
    mark("voice-set")

    -- One-note item. Reaper measures start/end in project seconds; the fixture
    -- renderer that drives Furnace places note-off at the same `note_off_s`
    -- relative position so the two waveforms line up in time.
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
