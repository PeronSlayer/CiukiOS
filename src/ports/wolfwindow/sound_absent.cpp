/* Explicit no-audio backend for the initial windowed Wolf source port. */
#include "wl_def.h"
boolean AdLibPresent=false,SoundBlasterPresent=false,SoundPositioned=false;
SDMode SoundMode=sdm_Off;
SDSMode DigiMode=sds_Off;
SMMode MusicMode=smm_Off;
int DigiMap[NUMSOUNDS],DigiChannel[NUMSOUNDS];
globalsoundpos channelSoundPos[8];
void SD_Startup(void) { for(int i=0;i<NUMSOUNDS;i++)DigiMap[i]=-1; }
void SD_Shutdown(void) {}
int SD_GetChannelForDigi(int) { return -1; }
void SD_PositionSound(int,int) {}
boolean SD_PlaySound(soundnames) { return false; }
void SD_SetPosition(int,int,int) {}
void SD_StopSound(void) {}
void SD_WaitSoundDone(void) {}
void SD_StartMusic(int) {}
void SD_ContinueMusic(int,int) {}
void SD_MusicOn(void) {}
void SD_FadeOutMusic(void) {}
int SD_MusicOff(void) { return 0; }
boolean SD_MusicPlaying(void) { return false; }
boolean SD_SetSoundMode(SDMode mode) { SoundMode=sdm_Off;return mode==sdm_Off; }
boolean SD_SetMusicMode(SMMode mode) { MusicMode=smm_Off;return mode==smm_Off; }
word SD_SoundPlaying(void) { return 0; }
void SD_SetDigiDevice(SDSMode) { DigiMode=sds_Off; }
void SD_PrepareSound(int) {}
int SD_PlayDigitized(word,int,int) { return -1; }
void SD_StopDigitized(void) {}
