#ifndef SONG_H

struct song {
	int bpm;
	int lps;
	int time_signature;
	int length;
	int drums[];
};

#define SONG_H
#endif
