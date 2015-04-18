#!/usr/bin/env python
import sys
import os
import xml.etree.cElementTree as et

name = os.path.splitext(os.path.basename(sys.argv[1]))[0]

tree = et.fromstring(sys.stdin.read())

song_data = tree.find('GlobalSongData')
bpm = int(song_data.find('BeatsPerMin').text)
lps = int(song_data.find('LinesPerBeat').text)
time_signature = int(song_data.find('SignatureNumerator').text) # XXX always 4

sequence = tree.find('PatternSequence').find('SequenceEntries')
patterns = tree.find('PatternPool').find('Patterns')


dctls = []
for e in sequence:
	pi = int(e.find('Pattern').text)
	p = patterns[pi]
	nl = int(p.find('NumberOfLines').text)
	ts = p.find('Tracks').findall('PatternTrack')
	dctl = [0] * nl
	for t in ts:
		for line in t.find('Lines') or []:
			index = int(line.get('index'))
			instr = int(line.find('NoteColumns').find('NoteColumn').find('Instrument').text)
			dctl[index] |= (1<<instr)
	dctls.extend(dctl)

#print bpm, dctls, len(dctls)
##print 

print "static struct song song_data_%s = {" % name
print "\t%d," % bpm
print "\t%d," % lps
print "\t%d," % time_signature
print "\t%d," % len(dctls)
print "\t{%s}" % ",".join(map(str, dctls))
print "};"
