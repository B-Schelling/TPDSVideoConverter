TPDSVideoConvert
================

This is a small tool to analyze and convert [Twitch Plays Dark Souls](http://www.twitch.tv/twitchplaysdark) videos (creating automated so called action-edits).

Twitch Plays Dark Souls is a 24/7 live stream that runs the game Dark Souls but every player input is decided by the live stream audience chat. In a very enduring way viewers vote every 30 to 40 seconds on the next 3 second move, changing this quick and highly reaction based action game to a Chess-like turn-based strategy game.

This tool works by reading Twitch archive streams directly, analyzing each frame to find all moving blocks and creating the finished resized & recompressed movie file in one single step. On my machine it processes an average of 330 FPS, thus converting 24 hours of video archive in a bit more than 2 hours.


Building
--------
You need a bunch of ffmpeg libraries to compile and run this (avformat, avcodec, avutil, swscale).

Easiest way to setup is to get Win64/Win32 development binaries from [Zeranoe](http://ffmpeg.zeranoe.com/builds/).

The Visual Studio project file and source code in this repository are assuming these to be in a sub-directory of the project directory called "ffmpeg" (containing lib, include, dll). If you place them somewhere else, change the paths in the vcxproj file and at the top of the source code file.

For platforms other than Win64/Win32, the bare-bone thread support at the top of the source code file needs to be adapted.


Usage
-----
To run the tool it needs to be fed video data via STDIN. I actually wrote a custom separate script which downloads Twitch past broadcast archives with a bunch of threads for maximum efficiency but I'm not sure if Twitch would want me to release that and also Livestreamer with some tweaked options might just be as good.

Using [Livestreamer](https://github.com/chrippa/livestreamer):

    livestreamer.exe -v --hls-segment-threads 5 "http://www.twitch.tv/twitchplaysdark/v/14796255" best -p "'TPDSVideoConverter.exe' output.mkv"

This creates output.mkv with all moving video parts and also a logfile named output.mkv.log.


Caveats
-------
The tool uses a rather simple method of detecting moving video, basically just checking the number of changed pixels each frame from a block of 30 frames and deciding where movement starts and stops. This leads to problems in places where actually something happens but visually not much changes on screen. This problem is mostly occurring in the game menu screens.
