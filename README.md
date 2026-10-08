<h1>Sébastien Loeb Rally Evo - Quality of Life Patch</h1>

*Majority of this was made with Claude to speed up automation of added values because they have to be added individually to every car, and adding support for multi input devices* but has been tested on my machine with my hardware. 

WARNING: Changing DATA.mix means that Online Multiplayer does not work, so if you want the Online Multiplayer Steam Achievements you need to run the original DATA.mix You will also get a error everytime you finish a stage that it cannot upload the time.

<h3>QOL Patch Contents:</h3> 

Windshield Fixes at night (LEZA MOD)

POV Centering for Cockpit Camera and Dashboard cam

Remove Steering Wheel + Hands

FOV Changer

Multiple Inputs working

Clutch setting actually enforcing the use of a clutch

Enable Speedometer in cockpit view

<h3>Prerequisites:</h3> 

Close the game before replacing CarShared or DATA.mix

You need any version of Python 3 installed on your computer

All python scripts should be placed in the games root folder SteamLibrary\steamapps\common\Sébastien Loeb Rally EVO

<h3>Step By Step Guide:</h3> 

<h4>*Windshield Fixes at night(LEZA MOD)*</h4>

For context after Milestone added Chinese language support, the windshield for some reason turned very pixelated at night, and was never fixed. This updated CarShared.mix files restores the pre-patch windshield.

Download the Patreon mod https://www.patreon.com/LezaKim/posts/sebastien-loeb-134603868?collection=1620499 (can be downloaded without an account)

The mod comes with two files DATA.mix and CarShared.mix. The only file we will use is CarShared.mix

CarShared.mix goes in the SteamLibrary\steamapps\common\Sébastien Loeb Rally EVO Folder\Cars folder

Take a backup of the old file before replacing

<h4>*POV Centering for Cockpit Camera and Dashboard cam*</h4>

Replace the default DATA.mix in the SLRE Steam folder with the one provided by this mod. This centers all the cameras to the center on both the Cockpit and Dashboard view (Defined as Sim view in the games files)

Take a backup of the original DATA.mix before replacing.

The mod also provides the python sript Campatch.py where you can change the values yourself

There are some cars that might appear slightly off center, they actually aren't the camera is perfectly centered it's just the steering wheel itself that's slightly off center. (Italian programming amirite)

<h4>*Remove Steering Wheel + Hands*</h4>

Removes the visible hands/wheel animation

Open CMD and change directory to your game directory example: E:\SteamLibrary\steamapps\common\Sébastien Loeb Rally EVO (cmd -> e: to change drive (replace with letter your game is installed on) then cd E:\SteamLibrary\steamapps\common\Sébastien Loeb Rally EVO\Cars)

py hidecockpit.py

Press enter 

Output will look like this:

driver / hands                   will change

steering wheel behaviour         will change

steering wheel visibility list   will change

Writing driver / hands ...

Backup written: DATA.MIX.bak

Writing steering wheel behaviour ...

Note: patched data is 832 bytes (slot is 813); storing it at the end of the file instead.
  
Writing steering wheel visibility list ...

Done. Driver/hands and steering wheel hidden.

<h4>*FOV Changer*</h4>

Changes the FOV in the Cockpit camera with option to change FOV in the dashboard view

Open CMD, change directory to your game directory example: E:\SteamLibrary\steamapps\common\Sébastien Loeb Rally EVO (cmd -> e: to change drive (replace with letter your game is installed on) then cd E:\SteamLibrary\steamapps\common\Sébastien Loeb Rally EVO\Cars)

py fovpatch.py xx (number for example 70. Default is 55) 

Press enter 

Output will look like this:

DATA\CAMERAS\COCKPITCAMERAS.BML: 59 DegFocal entries, slot at 0x3F41E0, 2088 bytes

Backup written: DATA.MIX.bak

Done: 59 entries in DATA\CAMERAS\COCKPITCAMERAS.BML now set to 70.

If you want to change the FOV of the Dashboard: py fovpatch.py 70 --file CAMERAS\SIMCAMERAS.BML

The backup file contains the old values

Start the game up and the FOV should be changed

<h4>*Multiple Inputs*</h4>

Take the files

dinput8_merge.ini

dinput8.dll

And put them in the root of the game for example:

E:\SteamLibrary\steamapps\common\Sébastien Loeb Rally EVO

Open dinput8_merge.ini

In devices on line 16 change the values that matches your wheel/pedal set. You can see the system names if you open joy.cpl from the windows search bar.

The game will then spoof your wheel/pedal set as a T300RS. The default is set for a Thrustmaster TS-PC and Fanatec Pedals which is what i own. 

I cannot guarantee this will work with every wheel/pedal set out there, but it's very easy to debug this with Claude if you run into problems

<h4>*Clutch working as intended & enable speedometer in cockpit view*</h4>

SLRE has always had a bindable clutch, but it was never required to shift even with the "H With Clutch" option selected in the game. This script fixes that, also allows you to enable clutch for the Manual setting that's made for sequential shifters. It replaces your SLRX64.exe with a new one + creates a backup

Enable speedometer in cockpit view is pretty self explanatory. The speedometer that is shown in every view except cockpit cam is now also shown there. 

Open CMD, change directory to your game directory example: E:\SteamLibrary\steamapps\common\Sébastien Loeb Rally EVO (cmd -> e: to change drive (replace with letter your game is installed on) then cd E:\SteamLibrary\steamapps\common\Sébastien Loeb Rally EVO\Cars)

Now decide on the following commands:

python slrexe.py (everything on (clutch for both paths + gauge in cockpit))

py slrexe.py --no-gauge (clutch only)

py slrexe.py --no-clutch (gauge only)

py slrexe.py --clutch-mode seq --always (changes the manual option in the game that uses paddles/sequential shifter to use clutch, the --always flag is necessary because the game normally registers this setting as not having a clutch required contrary to "H With Shifter" that has the flag but doesn't enforce it)

py slrexe.py --clutch-mode h (changes the option H with Shifter to require clutch)

py slrexe.py --threshold 0.5 (default = 0.25, allows you to adjust how much pedal pressure you need before the clutch activates)

py slrexe.py --undo (everything off, original game code)

<h3>*Extra tips for SLRE*</h3> 

Open the folder Sébastien Loeb Rally EVO\Movies

And remove Milestone.mp4 & Intro.mp4 to shorten startup time of the game

To remove input lag disable V-Sync in the games launcher, but use external software to cap the FPS at something you can run stable like 60fps. I use Rivatuner, but there are probably other options. 

Set wheel rotation to 900 in your wheels software, but 540 ingame. This will give you the correct steering angle.

<h3>Further development:</h3> 

I'd imagine with reverse engineering of the game with Claude Code, VR could be added to the game, and you could have a live FOV changer while the game is running, but since i do not own a VR headset, or am planning on paying for a Claude Code subscription i'm not gonna pursue this.

I'm also gonna look into if porting the stages from WRC 4 to the game is possible, and if i can add more championships to the game if that is the case

If you need help or want other changes, the fastest way to get Claude to do it is feed it a unpacked DATA.mix zip file, SLRX64.exe, the scripts provided by this mod and a screenshot of the games file structure

You can use this tool https://www.overtake.gg/downloads/mixfile-remixer.11107/ to unpack the data.mix and then zip it so you can upload it as one file to Claude.

<h3>Credits:</h3>

MixFile ReMixer by LeMic: https://www.overtake.gg/downloads/mixfile-remixer.11107/

Leza workplace for Windshield Fixes at night (LEZA MOD)

Patreon: https://www.patreon.com/collection/1620499

Youtube: https://www.youtube.com/@LezaKim

