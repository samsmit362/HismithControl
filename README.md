# Hismith Control With Funscripts Support
## About Project
This project is trying to fully reproduce described movements in funscripts by Hismith (not only speed) with video synchronization in VLC video player.\
After all steps required for it to work you will need only Start application and it will automatically detect what video do you play in VLC and execute related to it funscript with related scene synchronization.\
To be able to do this it uses computer vision with using your Web Camera which tracks Hismith rotation (angle position at each moment) and tries by manipulation with speed changes to achieve all required movements.\
For this moment it has very good results when speed is not too high, especially in slow movements (it can track very good all changes). In case of high speed 3-4+ strokes per second now it also has not so bad results.\
For minimize complexity of used computer vision algorithms, also as for increase accuracy and speed for making decision, it use color markers which you will need to place on Hismith (more details below)

## What is required for its work
You will need:\
Hismith with remote control support + Web Camera + backlight (Ring Light as very good solution) + PC + Add color markers to Hismith (which will be tracked)\
About Web Camera:\
I'm using very chip Web Camera but with Full HD support (1080p) 30fps (camera frame rate can greatly affects on how correctly it will know what is current angle rotation and also on speed for making decision) 
I'm using external backlight with Ring Light which is set to minimum. Web Camera is placed in the center of it .

## How to use
If you have a camera with external or integrated backlight you will need only to add color markers.\
I'm using for this blue and green insulating tapes (Multi Coloured Electrical Tape) which can be removed at any time. Also you will need that used colors mostly will not be present on Web Camera scenes.\
Example how to do this you can see in "example_images" folder.
So after getting all of this you will need:\
1) to run this program
2) to start Intiface Central https://intiface.com/central/ => https://github.com/intiface/intiface-central/releases/download/v2.6.0/intiface-central-v2.6.0-win-x64.exe
check that it can find your Hismith and you can change speed by using it.
3) press "Test Webcam", it will shows message which Web Cameras are available now, you will need to change settings.xml manually to Web Camera which you want to use by changing next field:
<req_webcam_name>Full HD webcam</req_webcam_name>
After restarting program try again: press “Test Webcam”
You will need to find what are best colors in your case (hotkeys will be listed in top) and manually update settings.xml next fields:\
<B_range>[10-160][150-205][60-120]</B_range>\
<G_range>[60-210][120-150][10-110]</G_range>\
If all is highlighted well you will need to go to next step
4) Press "Test Webcam+Hismith", by default it will start Hismith on mostly very low speed 5, which you can change to any other for check how good it can track all without errors.
After 10 seconds it will show in top average speed value which you can then manually update in: data\data.xml (now is used only rotation_speed_total_average which is most important but others still required for load settings but not used)\
<speed_data_average ... hismith_speed="5" ... rotation_speed_total_average="183" .../>\
...\
<speed_data_average ... hismith_speed="100" ... rotation_speed_total_average="1611" .../>\
For getting the best results it is greatly recommended to check all 5-100 speeds that rotation_speed_total_average are mostly the same or change them manually accordingly.
5) You will need special VLC build with milliseconds support for get best results (standard VLC now not supported it can sync only by seconds) you can find it in latest artifact in 3rdParty VLC fork project:\
https://code.videolan.org/skosnits/vlc-extended-playlist-support/-/artifacts \
Also you will need to enabler HTTP request supports in VLC according\
https://osr.wiki/books/funscript-playback/page/play-funscripts-using-vlc-and-multifunplayer \
Don't forget to align used settings with settings.xml fields:\
<vlc_url>http://127.0.0.1</vlc_url> \
<vlc_port>8080</vlc_port>\
<vlc_password>1234</vlc_password>\
If you correctly configured VLC you will can check this in Firefox by using http://127.0.0.1:8080/requests/status.xml \
(in my case Chrome doesn't open it)
6) So if all is done now you can simply press button "Start"
If VLC does not run it will show a message that is waiting for it.
When you select or open a video file (in playlist) which has funscript located near a video with the same name it will show "Ready to go" or "Running" depending from is video on pause or not.\
See available hotkeys and actions in the tray menu where the app will be hidden.\
ALt+B - pause\
ALt+N - continue\
ALt+Q - stop\
Also after play (Alt+Q) you can open new generated res_data\!results.txt for check results:\
dif_end_pos:5 req_dpos:180+(257) len:360 ... start_t:0:07:51:464 ...\
dif_end_pos:27 req_dpos:180+(135) len:238 ... start_t:0:07:51:824 ...\
dif_end_pos:-26 req_dpos:180+(147) len:320 ... start_t:0:07:52:062 ...\
dif_end_pos:-65 req_dpos:180+(83) len:240 ... start_t:0:07:52:382 ...\
dif_end_pos:-35 req_dpos:180+(183) len:360 ... start_t:0:07:52:622 ...\
...\
dif_end_pos is the most important part it show difference with what should be and what was gotten in angle of rotation, where '360' is full circle of rotation. \
 '-' means that movement was end later then it was in scene. \
 '+' means that movement was end early then it was in scene. \
 '-90' -- '90' are mostly very good results especially if they belo '45'.

## ! WARNING !
Be aware to stop Hismith by power button or hotkey (alt+q) to stop running.\
Due to different reasons like computer freeze or "Intiface Central" becoming unstable or "Funscript - some of which has very fast stroking on scenes where they are totally missed or not".\
So I highly recommend checking each scene before usage. You can simply play video with turned on program or use OpenFunscripter https://github.com/OpenFunscripter/OFS

## This project use next 3rdParty projects:
https://github.com/dumbowumbo/buttplugCpp \
https://github.com/studiosi/OpenCVDeviceEnumerator \
https://code.videolan.org/skosnits/vlc-extended-playlist-support









