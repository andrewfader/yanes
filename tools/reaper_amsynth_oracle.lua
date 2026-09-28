-- Reaper oracle driver for the amsynth-LV2 reference render.
--
-- Builds a one-track Reaper project hosting amsynth (LV2) playing a single
-- MIDI note. We use this to get an analog-style ADSR reference render; YANES
-- goes through the in-process CLAP harness so its parameter binding isn't
-- obscured by the Reaper-to-CLAP value-mapping path. See
-- tools/test_amsynth_envelope_oracle.sh for the larger rationale.

local project_path    = assert(os.getenv("YANES_REAPER_PROJECT"),    "YANES_REAPER_PROJECT is required")
local marker_path     = assert(os.getenv("YANES_REAPER_MARKER"),     "YANES_REAPER_MARKER is required")
local render_dir      = assert(os.getenv("YANES_REAPER_RENDER_DIR"), "YANES_REAPER_RENDER_DIR is required")
local note_key        = tonumber(os.getenv("YANES_REAPER_NOTE_KEY"))    or 60
local note_velocity   = tonumber(os.getenv("YANES_REAPER_NOTE_VEL"))    or 100
local note_off_s      = tonumber(os.getenv("YANES_REAPER_NOTE_OFF"))    or 0.6
local render_end_s    = tonumber(os.getenv("YANES_REAPER_RENDER_END"))  or 2.0
local release_norm    = tonumber(os.getenv("YANES_REAPER_RELEASE"))     or 0.4

local function mark(text)
    local file = assert(io.open(marker_path, "w"))
    file:write(text, "\n")
    file:close()
end

local function run()
    mark("started")
    reaper.InsertTrackAtIndex(0, true)
    local track = reaper.GetTrack(0, 0)
    reaper.GetSetMediaTrackInfo_String(track, "P_NAME", "amsynth oracle", true)

    -- amsynth LV2: try the explicit display name, then a name substring match.
    local fx = reaper.TrackFX_AddByName(track, "LV2i: amsynth (Nick Dowell) (2 out)", false, -1)
    if fx < 0 then fx = reaper.TrackFX_AddByName(track, "amsynth", false, -1) end
    if fx < 0 then mark("error: cannot instantiate amsynth"); return end

    for i = 0, 60 do
        local retval, name = reaper.TrackFX_GetParamName(track, fx, i, "")
        if not retval then break end
        local s = (name or ""):lower()
        if     s:find("attack")  then reaper.TrackFX_SetParamNormalized(track, fx, i, 0.0)
        elseif s:find("decay")   then reaper.TrackFX_SetParamNormalized(track, fx, i, 0.0)
        elseif s:find("sustain") then reaper.TrackFX_SetParamNormalized(track, fx, i, 1.0)
        elseif s:find("release") then reaper.TrackFX_SetParamNormalized(track, fx, i, release_norm)
        end
    end
    mark("amsynth-loaded")

    local item = reaper.CreateNewMIDIItemInProj(track, 0.0, render_end_s, false)
    local take = reaper.GetActiveTake(item)
    local start_ppq = reaper.MIDI_GetPPQPosFromProjTime(take, 0.0)
    local end_ppq   = reaper.MIDI_GetPPQPosFromProjTime(take, note_off_s)
    reaper.MIDI_InsertNote(take, false, false, start_ppq, end_ppq, 0, note_key, note_velocity, true)
    reaper.MIDI_Sort(take)
    mark("midi-set")

    reaper.GetSetProjectInfo(0, "PROJECT_SRATE", 48000, true)
    reaper.GetSetProjectInfo(0, "RENDER_SRATE", 48000, true)
    reaper.GetSetProjectInfo(0, "RENDER_CHANNELS", 2, true)
    reaper.GetSetProjectInfo(0, "RENDER_BOUNDSFLAG", 0, true)
    reaper.GetSetProjectInfo(0, "RENDER_STARTPOS", 0.0, true)
    reaper.GetSetProjectInfo(0, "RENDER_ENDPOS", render_end_s, true)
    reaper.GetSetProjectInfo_String(0, "RENDER_FILE", render_dir, true)
    reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", "yanes-amsynth-amsynth", true)
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
