SUPER HANG-ON ULTIMATE - META QUEST (STANDALONE) BUILD
======================================================

This folder builds the game as an app that runs on the headset itself (Quest 2, Pro, 3, 3S), with the same
stereo 3D picture and handlebar controls as the Windows VR mode. It is a private build: the game code in
source\src\gen was made from your own ROM, so do not pass the project or the APK on.

WHAT YOU NEED
  - A Windows PC, about 3 GB of free space, an internet connection for the first build.
  - The Quest in developer mode, and any way of sideloading an APK. USB is not needed.
  - Your shangon.zip (and, for Original Mode, your Mega Drive ROM): put copies next to build-quest.ps1.
    The script packs them inside the APK, so nothing has to be copied to the headset by hand.

BUILD AND INSTALL
  1. Right-click build-quest.ps1 > Run with PowerShell.
     (If Windows blocks it: open PowerShell in this folder and run
        powershell -ExecutionPolicy Bypass -File .\build-quest.ps1 )
  2. The first run downloads Java, Google's Android tools and the OpenXR loader into the "tools" folder here.
     Type y and Enter when Google's licences are shown.
  3. It compiles and makes SuperHangOnQuest.apk in this folder, with your ROMs inside.
  4. Sideload SuperHangOnQuest.apk your usual way. (If the headset is connected by USB with debugging allowed,
     the script installs it itself.)
  In the headset: Library > "Unknown sources" > Super Hang-On Ultimate.

THE GAME'S FOLDER ON THE HEADSET
  Quest > Internal shared storage > Android > data > com.shangon.ultimate > files
    shangon.zip            your arcade ROM (unpacked from the APK on the first start)
    <Mega Drive ROM>       optional: turns on Original Mode (likewise)
    music\                 optional: the four arranged MP3s, as on the PC
    shangon.ini            settings (made by the game; the menus save into it)
    shangon.log            speed and sound report, a line every 30 seconds - send this if something is wrong

THE LAUNCHER (the settings screen, as on the PC)
  Left stick up / down = choose a line, left / right = change it, A = change / PLAY, Menu = PLAY.
  Demo sounds, difficulty, time, screen shape, music, Mega Drive Enhancements (Original Mode) and Mega Drive
  Controls are set here; they are kept for next time. To skip it, set  show=0  under [launcher] in shangon.ini.

CONTROLS (the same as the Windows VR mode)
  Hold the two controllers like handlebars: lower one hand to lean that way.
  Right trigger = throttle, left trigger = brake, grip = supercharger.
  A / B = select / back, X = coin, Menu (left controller) = start / pause.
  Y = settings: 3D depth and pop out, screen height / distance / width, steering (tilt or stick),
      throttle (trigger or twist grip), triggers swapped.
  Right stick up / down = move the screen; right stick click = recentre it in front of you.

IF IT DOES NOT WORK
  - A message in the headset saying shangon.zip is not there: it was not next to build-quest.ps1 when the APK
    was made. Put it there and build again.
  - A black screen or the app closing at once: describe exactly what you see. (With a USB connection the
    headset's own log can be read:  tools\sdk\platform-tools\adb.exe logcat -d -s SuperHangOn:* > log.txt )
  - Slow or stuttering: shangon.log in the game's folder has the numbers.

To remove everything from the PC, delete this folder. To remove the game from the headset: uninstall it in
Library > Unknown sources (that also deletes its folder).
