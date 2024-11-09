# Hismith Control With Funscripts Support
## About Project
This project is trying to fully reproduce described movements in funscripts by Hismith (not only speed) with video synchronization in VLC video player.\
After all steps required for it to work you will need only Start application and it will automatically detect what video do you play in VLC and execute related to it funscript with related scene synchronization.\
To be able to do this it uses computer vision with using your Web Camera which tracks Hismith rotation (angle position at each moment) and tries by manipulation with speed changes to achieve all required movements.\
For this moment it has very good results when speed is not too high, especially in slow movements (it can track very good all changes). In case of high speed 3-4+ strokes per second now it also has not so bad results.\
For minimize complexity of used computer vision algorithms, also as for increase accuracy and speed for making decision, it use color markers which you will need to place on Hismith (more details below)

## What is required for its work
You will need:\
Hismith with remote control support (or possibly any other similar form Fucking Machine) + Web Camera + backlight (Ring Light as very good solution) + PC + Add color markers to Hismith (which will be tracked)\
About Web Camera:\
I'm using very chip Web Camera but with Full HD support (1080p) 30fps (camera frame rate can greatly affects on how correctly it will know what is current angle rotation and also on speed for making decision) 
I'm using external backlight with Ring Light which is set to minimum. Web Camera is placed in the center of it .

## How to use
**If you have a camera with external or integrated backlight you will need only to add color markers.\
I'm using for this blue and green insulating tapes (Multi Coloured Electrical Tape) which can be removed at any time.\
Also you will need that used colors mostly will not be present on Web Camera scenes.\
Example how to do this you can see in "example_images" folder.**\
So after getting all of this you will need:
1) to start Intiface Central https://intiface.com/central/ => https://github.com/intiface/intiface-central/releases/download/v2.6.0/intiface-central-v2.6.0-win-x64.exe
check that it can find your Hismith and you can change speed by using it.
2) to run this program
3) select Web Camera in combo box which you wish to use for detect Hismith rotation angle.\
**Press "Test Webcam". According images in "example_images" folder you will need to find what are best colors in your case (don't forget to save all changes by menu or Ctrl+S):**\
<code><B_range>[10-160][150-205][60-120]</B_range>\
<G_range>[60-210][120-150][10-110]</G_range></code>\
**B_range of colors (highlighted as Blue during test) is used for detection:**
- left corner edge of Hismith
- center of rotation (for detect it's placement)
- and for right connection of moving telescopic motor rocker arm\
**G_range of colors (highlighted as Green during test) is used for detection:**
- rotation part of telescopic motor rocker arm which perform circular movements.\
If all is highlighted well you will need to go to next step
4) Select Hismith device in the combo box or some similar which you plan to use. Press "Test Webcam+Hismith".\
By default it will start Hismith on mostly very low speed 5, which you can change to any other for check how good it can track all without errors.\
After 10 seconds it will show in top average speed value.
5) If you plan to use other device or for getting the best results **it is highly recommended to run "Get Statistics Data"** (also it is recommended to run on same configuration which you mostly plan to use), it will takes ~20 minutes.\
In this case it will automatically run Hismith from 1-2-3-to-"Hismith Speed Limit" (max_allowed_hismith_speed in settings.xml) (by default 50) for about 3+7 seconds on each speed.\
So if you plan to use a higher speed than 50 you need to set "Hismith Speed Limit" to 100 or lower.
6) You can also run "Test Performance" in my case avg_dt_\* results are ~42 (milliseconds) max_dt_\* ~67-93 and it is enough for get good results.\
Than low values than more accurate and often application will can control the device.
7) **Highly recommended to use special VLC build with milliseconds support for get best results** (standard VLC currently doesn't support it, it can sync only by seconds) you can find it in the latest artifacts in 3rdParty VLC fork project:\
https://code.videolan.org/skosnits/vlc-extended-playlist-support/-/releases \
You can also use standard VLC but be aware that desynchronization of Hismith moves and video timeline can be ~500-1000 milliseconds.\
Also you will need to enabler HTTP request supports in VLC according\
https://osr.wiki/books/funscript-playback/page/play-funscripts-using-vlc-and-multifunplayer \
Don't forget to align used settings with settings.xml fields:\
<code><vlc_url>http://127.0.0.1</vlc_url>\
<vlc_port>8080</vlc_port>\
<vlc_password>1234</vlc_password></code>\
If you correctly configured VLC you will can check this in Firefox by using http://127.0.0.1:8080/requests/status.xml \
(in my case Chrome doesn't open it)
8) So if all is done now you can simply press button "Start".\
In this case program will be minimized to tray, but it will notify
users about it's status by popup messages shown topmost on the middle of screen, they will not break your current mouse focus, so you can play video in fullscreen without issues with hotkeys etc.\
If VLC does not found it will show a message that it's waiting for it.\
When you select or open a video file (in playlist) which has funscript located near a video file with the same base name it will show "Ready to go" or "Running" depending from is video on pause or not.\
See available hotkeys and actions in the tray menu where the app will be hidden.\
**ALt+B - pause\
ALt+N - continue\
ALt+Q - stop**\
Also after play (Alt+Q) you can open new generated res_data\\!results.txt for check results:\
<code>**dif_end_pos:-19** start_t:0:07:50:424 len:280 req_dpos:180+(52) ...\
**dif_end_pos:-15** start_t:0:07:50:704 len:400 req_dpos:180+(114) ...\
**dif_end_pos:-6** start_t:0:07:51:104 len:360 req_dpos:180+(87) ...\
**dif_end_pos:4** start_t:0:07:51:464 len:360 req_dpos:180+(94) ...\
**dif_end_pos:-12** start_t:0:07:51:824 len:238 req_dpos:180+(55) ...\
**dif_end_pos:0** start_t:0:07:52:062 len:320 req_dpos:180+(137) ...\
**dif_end_pos:-15** start_t:0:07:52:382 len:240 req_dpos:180+(75) ...\
**dif_end_pos:-10** start_t:0:07:52:622 len:360 req_dpos:180+(122) ...\
**dif_end_pos:-24** start_t:0:07:52:982 len:320 req_dpos:180+(47) ...\
**dif_end_pos:0** start_t:0:07:53:302 len:361 req_dpos:180+(113) ...\
...</code>\
**dif_end_pos** is the most important part it show difference with what should be and what was gotten in angle of rotation, where '360' is full circle of rotation. \
 '-' means that movement was end later then it was in scene. \
 '+' means that movement was end early then it was in scene. \
 '-90' -- '90' are mostly good results especially if they below '45'.

## <font color="red">! WARNING !</font>
**<font color="red">Be aware to stop Hismith by power button or hotkeys to stop running.</font>**\
Due to different reasons like computer freeze or "Intiface Central" lose Hismith device or "Funscript - some of which has very fast stroking on scenes where they are totally missed or not".\
If you are not sure about script or fear to get injury you can limit max speed in program by changing "Hismith Speed Limit" in GUI.\
So I highly recommend checking each scene before usage.\
**To minimize risks I have initially set "Hismith Speed Limit" to 50 (50%) by default, but you will get less good experience in this case.**\
You can simply play video with turned on program for check how it work or use OpenFunscripter https://github.com/OpenFunscripter/OFS to check script on high intensity moves.\
**For get max good expirience but wholly on you risk it is recommended to set "Hismith Speed Limit" to 100 (100%) and try to use with turned "off" and "on" (both variants) "Use Modify Funscript Functions" CheckBox but don't forget to check on each scene before usage**

## Notes
Min Funscript Relative Move - is used for modify funscript actions, if move change from up to down (or vice versa) according funscript are lover then "Min Funscript Relative Move"
it will try to find the first next move with which min to max positions will be >= "Min Funscript Relative Move" if such found it will combine from min to max all actions with averaging values in move.
For more details which actions was averaged you can see in "res_data\\!results_for_get_parsed_funscript_data.txt"\
Also program save result parsed funscript to "res_data" folder with same name as original file used.

## Modify Funscript Functions
You can use this option for get better experience on simple moves.\
In most cases funscripts use simple patterns for "in"(going inside) and "out"(going outside) moves without details how to make it (without additional points inside such moves).\
In case of turn on this feature ("Use Modify Funscript Functions" CheckBox) it will automatically replace such simple moves by adding additional move detail points inside moves.\
You can turn on/off or switch used Modify Funscript Functions even during execution by using hotkey (<hotkey_use_modify_funscript_functions> in settings.xml).\
\
Variants of such added points are defined in "Functions move variants:" GUI (<functions_move_variants> in settings.xml).\
Its format is:\
[ddt(0.0-1.0):ddpos[0.0-1.0]|ddt(0.0-1.0):ddpos[0.0-1.0]|...],[ddt(0.0-1.0):ddpos[0.0-1.0]|ddt(0.0-1.0):ddpos[0.0-1.0]|...],...\
or\
[unchanged],...\
where:\
ddt is relative time offset in percents from "move start time" to "move end time" and whose value should be in range (0.0, 1.0)\
ddpos is relative move offset in percents from "move start position" to "move end position" and whose value should be in range [0.0, 1.0]\
unchanged is used in case of random move generation and allow to randomly skip changes for some moves.\
For example:\
<functions_move_variants>[unchanged],[0.25:0.38|0.75:0.62],[0.25:0.12|0.75:0.87]</functions_move_variants>\
Define three move variants:\
First variant (id == 1): [unchanged] use move variant without changes\
Second variant (id == 2): [0.25:0.38|0.75:0.62] which by adding two inside points defines move details pattern like: [fast:slow:fast]\
Third variant (id == 3): [0.25:0.12|0.75:0.87] which by adding two inside points defines move details pattern like: [slow:fast:slow]\
\
For define which variant to use for concrete move types "in"(going inside) or "out"(going outside) are used\
"Functions move in/out variants:" in GUI (selected sub variant in ComboBox will be used)\
<functions_move_in_out_variants> and <functions_move_in_out_variant>(which contains selected sub variant ID) in settings.xml\
Its format is:\
[min_speed_in_rpm_1-max_speed_in_rpm_1:move_in_variant_id_1_1/move_out_variant_id_1_1|(or)move_in_variant_id_1_2/move_out_variant_id_1_2|...],\
[min_speed_in_rpm_2-max_speed_in_rpm_2:move_in_variant_id_2_1/move_out_variant_id_2_1|(or)move_in_variant_id_2_2/move_out_variant_id_2_2|...],...;\
[min_speed_in_rpm_1-max_speed_in_rpm_1:move_in_variant_id_1_1/move_out_variant_id_1_1|(or)move_in_variant_id_1_2/move_out_variant_id_1_2|...],\
[min_speed_in_rpm_2-max_speed_in_rpm_2:move_in_variant_id_2_1/move_out_variant_id_2_1|(or)move_in_variant_id_2_2/move_out_variant_id_2_2|...],...\
if simple move has average speed in rpm in range \[min_speed_in_rpm_\"id\", max_speed_in_rpm_\"id\"\] then random "in/out" move variant pair\
will be used from \[move_in_variant_id_\"id\"\_1/move_out_variant_id_\"id\"\_1|(or)move_in_variant_id_\"id\"\_2/move_out_variant_id_\"id\"\_2|...\]\
where:\
speed in rpm - is number of strokes per minute\
min_speed_in_rpm_\"id\" should be >= 0\
max_speed_in_rpm_\"id\" should be > 0 and can be set to "maximum"\
move_(in(or)out)_variant_id_\"id\"\_\"sub_id\" should take value from 1 to "number_of_move_variants_in_<functions_move_variants>" or set to "random"\
For example:\
<functions_move_variants>[0-200:2/3|3/2|random/random],[200-maximum:1/1];[0-200:3/2]</functions_move_variants>\
<functions_move_variant>1</functions_move_variant>\
If simple move type is "in" and its average speed in rpm > 0 and <= 200 (in range [0, 200]) then\
additional detail points will be added from move variant with id == 2 (according pair: 2/3) or id == 3 (according pair: 3/2) or 'random' (according pair: random/random) randomly.\
If simple move type is "out" and its average speed in rpm > 0 and <= 200 (in range [0, 200]) then:
- if previous was simple move "in" with average speed in range [0, 200] and was selected move variant from pair '3/2' then\
additional detail points will be added from move variant with id == 2 (according pair: 3/2)
- if previous move was not "simple move 'in' with average speed in range [0, 200]" then\
additional detail points will be added from move variant with id == 3 (according pair: 2/3) or id == 2 (according pair: 3/2) or 'random' (according pair: random/random) randomly.

## Known issues
Sometimes even when Hismith device is found on "Test Webcam+Hismith" after press "Start" it still show issue that can't find device or etc, known solution is to reboot OS.\
\
The most complex in case of automatic Hismith device control is to stop its rotation, according experiments there is a time delay ~150 milliseconds between setting speed change and real device speed changes appearance.
Also if device is run on high speed it can takes about a half of second for full stop even when speed is set to 0. In these cases **dif_end_pos** can be > 90 and some times > 180.

## This project use next 3rdParty projects:
https://github.com/dumbowumbo/buttplugCpp \
https://github.com/studiosi/OpenCVDeviceEnumerator
