#pragma once

// Two editions build from this source (CMake option GMOD_REPLAY_EDITION):
//   private - JoeyMarrey29/gmod-replay: only usable by a signed-in repository collaborator.
//   public  - JoeyMarrey29/gmod-replay-public: no sign-in; recent recordings stay locked.
namespace gmod::edition {

#ifdef GMOD_REPLAY_PUBLIC
inline constexpr bool is_public = true;
inline constexpr wchar_t state_folder[] = L"GModReplayPublic";
inline constexpr wchar_t app_mutex[] = L"Local\\GModReplayPublic.Application";
#else
inline constexpr bool is_public = false;
inline constexpr wchar_t state_folder[] = L"GModReplay";
inline constexpr wchar_t app_mutex[] = L"Local\\GModReplay.Application";
#endif

} // namespace gmod::edition
