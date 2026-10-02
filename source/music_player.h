#ifndef INFINITY_BLADE_NX_MUSIC_PLAYER_H
#define INFINITY_BLADE_NX_MUSIC_PLAYER_H

void music_player_set_volume(float volume);
int music_player_play(const char *song_name);
void music_player_stop(void);

#endif

