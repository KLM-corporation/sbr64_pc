// Background music for the JNI MediaPlayer calls (PlayMusic/PauseMusic/ResumeMusic/RestartMusic/StopMusic/
// SetMusicVolume). The original game plays one MP3 from res/raw at a time; here the MP3 is decoded with
// Media Foundation on a worker thread into a ring of 44.1 kHz stereo PCM that the OpenSL mixer pulls from.
#pragma once

#include <cstdint>
#include <string>

namespace zb::audio::music {

constexpr int kRate = 44100;

// Folder holding gardens.mp3, canyons.mp3, avalonia.mp3, home.mp3, homexmas.mp3 and credits.mp3.
void set_music_dir(const std::string& dir);

// Called when a track starts so that the audio output (the OpenSL mixer) is running.
void set_output_starter(void (*fn)());

// MediaPlayer semantics: replaces the current track. Returns false for an unknown track name.
bool play(const std::string& track, bool loop);
void pause();
void resume();
void restart();  // seek to the start and play
void stop();     // release the player
void set_volume(float v);

// Window focus: silences the output without touching the game's own pause state.
void set_suspended(bool suspended);

bool is_playing();

// Mixer side: adds `frames` interleaved stereo frames of the current track into `acc`.
void mix_into(int32_t* acc, int frames);

}  // namespace zb::audio::music
