#ifndef SONG_H

struct song {
	int bpm;
	int lpb;
	int time_signature;
	int length;
	int drums[];
};

#define SONG_H
#endif
