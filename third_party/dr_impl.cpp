// Single translation unit holding the dr_libs implementations used by Bulkhead (decoding
// the sound effects: MP3 from ElevenLabs, WAV for anything else).
#define _CRT_SECURE_NO_WARNINGS
#pragma warning(push, 0)
#define DR_MP3_IMPLEMENTATION
#include <dr_mp3.h>
#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>
#pragma warning(pop)
