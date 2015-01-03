Recolino - a real-time command line annotator
=============================================

SYNOPSIS
--------

	recolino [-a | --audioport jack_monitor_port] [-m | --midiport jack_midi_input_port] audiofile

DESCRIPTION
-----------

Recolino is a small command line utility for the annotation of audio files by
listening and simultaneously tapping, either on the PC keyboard or a MIDI
device. 

It builds on [JACK](http://jackaudio.org) for audio playback and MIDI input.

Whenever Recolino receives a tapping event it will print out a timestamp and a
label. The timestamp in seconds relates to the beginning of the audio file and
the label is either the character that was pressed on the PC keyboard or the
MIDI note number of a MIDI note-on event.  

	Keyboard input   MIDI input
	...              ...
	11.775 a         11.763 39
	12.196 s         12.175 48
	12.619 d         12.589 45
	13.025 f         13.007 43
	13.431 a         13.403 39
	13.852 s         13.819 48
	14.266 d         14.244 45
	14.688 f         14.643 43
	15.119 a         15.080 48
	15.572 s         15.491 39
	15.978 d         15.943 39
	16.335 f         16.376 39
	...              ...

It can, for example, be used to build up a ground-truth database for beat and
down-beat tracking.  Nowadays, the weapon of choice for such a task is probably
[Sonic Visualiser](http://www.sonicvisualiser.org) or maybe the 
[CLAM Music Annotator](http://clam-project.org/wiki/Music_Annotator). 
However, Recolino has a much smaller footprint and suits the workflow of
command line-based annotation sessions very well.

An exemplary Bash script (anndir):

	#!/bin/bash
	
	if [ $# -lt 1 -o ! -d "$1" ]; then
		echo "usage: $(basename $0) audiodir [format monitorport midiport outsuffix]"
		exit
	else
		dir=${1%/}
	fi
	
	# default format is wav
	format=${2:-wav}
	monitorport=${3:-system:playback_1}
	midiport=${4:-alsa_pcm:nanoPAD/midi_capture_1}
	outsuffix=${5:-ann}
	
	# exit if no files match *.$format
	shopt -s failglob
	
	# to deal with file names containing spaces
	export IFS=$'\n'
	
	for f in $dir/*.$format; do
		# pipe output to working directory
		outfile=$(basename ${f%.$format}.$outsuffix)
		if [ -e "$outfile" ]; then
			read -p "'$outfile' exists, overwrite? (y|n) " ow
			if [ "$ow" != "y" ]; then continue; fi
		fi
		action=r
		while [ "$action" != "p" ]; do
			recolino -a $monitorport -m $midiport $f > $outfile
			# what's next?
			read -p "Repeat, proceed, quit? (r|p|q) " action 
			if [ "$action" = "q" ]; then break 2; fi
		done
	done

Calling `./anndir /data/audio flac` will start a session to annotate all flac
files in `/data/audio`. The annotation for `file.flac` is saved to `file.ann`
in the current working directory.

Note that the MIDI port name is a JACK MIDI name, which means you will probably
want to add `-Xseq` to jackd's options in order to forward ALSA sequencer ports
corresponding to your hardware to JACK. (As you can notice above, I use a Korg
nanoPAD.)

There is only a single monitor port. All channels are mixed to mono.

BUILDING
--------

Recolino is written in C and depends on (a reasonably recent version of) JACK,
libsndfile, and termios.h. These dependencies can easily be fulfilled by a
recent Linux distribution. Porting to other platforms (OS X?) might be
possible, though. There is no configure script for now, only a simple Makefile.

LICENSE
-------

Recolino is free software, licensed under the terms of the GPL2.

DOWNLOAD
--------

<http://www.jawebada.de/Software/Recolino>

