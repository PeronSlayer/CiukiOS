
                           Quake III Arena Demo
                               Version 1.11
                            December 6th, 1999




       TABLE OF CONTENTS

       1 System Requirements
       2 X11 Setup 
       3 Sound Setup
       4 Playing The Game
       5 Known Issues
       6 Contacting id Software
       7 Contacting Loki Entertainment Software


-------------------------------
(1) MINIMUM SYSTEM REQUIREMENTS
-------------------------------

  * Linux kernel version 2.2.9 or above, glibc-2.0 or above
  * XFree86 SVGA server version 3.3.5 or above, running at 16 bpp depth
  * Pentium 233MHz MMX processor with 3dfx 8 MB Video Card
     or Pentium II 266MHz processor with 3dfx 4 MB Video Card
     or AMD 350MHz K6-2 processor with 3dfx 4 MB Video Card
  * 64 MB RAM
  * 4x CD-ROM drive (600 KB/sec sustained transfer rate)
  * OSS compatible sound card
  * Hard disk drive with at least 20MB of disk space,
     and 480MB if you do not want to use the live filesystem from CD

  Multiplayer Requirements
  * Internet and LAN (TCP/IP) play supported
  * Internet play requires a 28.8 Kbps (or faster) modem

A 100% full OpenGL compliant 3-D video card and Linux driver is 
required. Quake III Arena uses the OpenGL API for 3-D hardware 
acceleration. There is no software-only version of the game. If
your computer is not hardware accelerated with a game compatible
graphics card, you will NOT be able to run the Q3A demo. Currently,
only drivers for 3dfx based cards are included in the demo install.
For updates on GL drivers, in particular for the Matrox G200/G400,
see http://www.lokigames.com/support/gldrivers/.

For 3dfx based products, Linux Quake III Arena Demo is distributed
with a version of Mesa 3-D specifically compiled for Voodoo support.
You must have 3Dfx Linux Glide drivers installed however. These
drivers are available from:

		http://www.3dfxgamers.com/

Make sure that you download and install the glibc versions of the drivers
(found at the bottom of the page). Do not use the XFree86 DRI pre-release
if you are looking for drivers compatible with the current stable XFree86
3.3.5 release.

NOTE TO Voodoo3 XFree86 3.3.5 users:  Make sure you install the X Server
listed on the 3dfx Linux Drivers page.  The 3.3.5 XFree86_SVGA Server that
ships with most Linux distributions has problems with DGA Mouse handling.

Please follow the installation instructions presented there for adding the
correct drivers for your 3D-acceleration card. Note that there are two ways
to install the 3dfx drivers-one with support for a /dev/3dfx device that
does not require root permissions in order to use the card. If you choose to
not use the /dev/3dfx method, you will have to run Q3A Demo as the root user.

NOTE:  Linux Q3A Demo will try to load "libGL.so" before using 3dfx drivers.
This can cause problems if you have a software or third party OpenGL driver
installed.  You can specifically target the included 3dfx based Mesa driver
by using the following command line:

	./q3demo +set r_gldriver libMesaVoodooGL.so.3.2

For 3dfx users, you may disable the vertical sync refresh.  This can improve
proformance at the cost of some visual tearing of the image.  Entering the
following command into your shell before running Linux Q3A Demo will turn off
the vertical sync:

	export FX_GLIDE_SWAPINTERVAL=0

Then run Linux Q3A Demo from the same command line normally.

A glibc compatible Linux installation is required.  An easy to determine
if you have glibc support is to type this:

	ls -l /lib/libc*

If you get a report of libc6 (you may also have libc5), you have a glibc 
based system. The binaries were created on a RedHat 5 (glibc-2.0.7) system.


--------------
(2)  X11 SETUP
--------------
Linux Q3A Demo requires X11 to run. There is no console-based version as in
previous id products such as GLQuake and Quake2. XFree86 version 3.2 or
later is required.

There are two ways that mouse input is handled under XFree86:

        o By default, Q3A Demo will attempt to use DGA mouse handling. DGA
          support features direct reading of the mouse motion and provides
          more accurate control while playing the game. By default this
          support is enabled, but can be disabled by adding 
           "+set in_dgamouse 0" 
          to the command line at startup. This is recommended if you are
          using a Voodoo3 and are experiencing black screens, lockups,
          or mapping of mouse input to key events. This bug should be
          resolved on XFree86 SVGA 3.3.5 (except for a blue mouse cursor
          you might notice on the initial fullscreen frame).

        o The non-DGA method of mouse input uses pointer grabbing and warps
          the pointer to the middle of the window on each mouse update. On
          systems with a slow frame rate and a lot of mouse user input, the
          motion can get "clipped" to the window boundaries. This method of
          input is more compatible however.

Q3A Demo uses the XFree86 VidModeExtension facilities if available to provide
fullscreen play. This does not apply to 3Dfx passthrough based cards, since
the passthrough cable takes over the video display upon activation anyway.

When configuring your X11 server, make sure that you include lower
resolution modes such as 640x480 and 800x600. Q3A Demo will auto-switch to
these modes using the VidModeExtension if you select fullscreen from the
graphics options menu. If the lower resolution modes are not listed in the
XFree86 configuration file, Q3A Demo will be unable to switch to the desired
resolution for fullscreen play. Instead, it will using Mesa's software mode
to render into a window (very slow), or might fail to run at all, possibly
putting you in a different video in X11 on exiting.


---------------
(3) SOUND SETUP
---------------
Q3A Demo uses the /dev/dsp sound device for sound support under Linux. This 
is the default device provided by the sound drivers included with the Linux
kernel. Please note that at the time of this writing, PCI based sound cards
such as the SoundBlaster Live and Diamond Monster MX series were not
supported. They may be supported in the future. Check
   
  http://www.opensound.com/ 

for support in the future.

If you do not run Q3A Demo as root (recommended), you may need to make sure
the permissions for the /dev/dsp device are read and write by the user
running Q3A Demo. The cheap and easy way to make this happen is to do 

  "chmod o+rw /dev/dsp" 

as root. For the more security conscious, a special sound group could be 
created and Q3A Demo could be made setgid to the sound group to access the 
device.

Q3A Demo uses mmap() to map the sound buffers on /dev/dsp directly in order 
to provide responsive sound needs. Sound cards must be able to support this
feature in order to work. SoundBlaster 16, AWE32 and AWE64 cards are known
to work.



--------------------
(4) PLAYING THE GAME
--------------------

By the time you read this, you will probably have succeeded in installing
from the self-extracting archive. Just run "q3demo" to start the game. 

It is recommended that you do not run Q3A Demo using the root account or 
with root privileges. There are two exceptions to this:

        o If you are using a 3Dfx based accelerator card and do not install
          the /dev/3dfx configuration option. (You will have to run as root
          in order to access the card).
        o If you do not have access to the /dev/dsp device and do not wish
          to change the mode of the device so that non-root accounts can
          access it.

You will have to be running under X11 or have the DISPLAY variable pointed
to a OpenGL glX capable X Server.

By default, Q3A Demo tries to find the following OpenGL libraries in this
order:

        o libGL.so
        o libMesaVoodooGL.so

You can override the library name by entering, "+set r_gldriver <libname>"
on the command line. This may be needed if you are using a non-standard set
up and have a different name for the OpenGL shared library. If you do not
specify a valid GL library, or if the GL library specified can't find or
access the hardware it expects, Mesa might fall back into slow software
rendering mode (usually in a window).

If everything proceeds successfully, you should have a Q3A Demo window on 
your desktop with a menu displayed (3dfx card owners will get a full screen
view). If you want to use full screen, go to the System Configuration, 
Graphics, Options, Fullscreen, change the value to Yes, and hit enter to 
apply it.

If you intend to connect to the Internet to play Q3A Demo, make certain
that your net connection is open and working first.

The first time you run Quake III Arena it will create a .q3a/ directory 
in your home directory.  Saved games and preferences will be stored here.

The demo offers an optional install of the HTML version of the manual, 
and more technical details on how to use Linux Quake III Arena. See
the Help/ folder.


----------------
(5) KNOWN ISSUES
----------------

Q. My mouse is erratic. My player is constantly looking up or down and
I can't seem to move him.

A. This happens with two issues. Some versions of XFree86 have a bug in
DGA mouse handling that confuses Q3A. You can try turning off DGA mouse
support by entering "+set in_dgamouse 0" on the command line when you
start Q3A Demo. This problem can also occur after a restart of the video
system (such as after a graphic setting change). This is because some
sort of event occurred to jam up the mouse. Restarting the video system
another time and not moving the mouse during the restart should clear it.


Q. When I do a video restart (for example, selecting Apply in the Graphics
Options menu), Q3A Demo exits with a Sig11.

A. This is a driver related problem.  
On 3dfx hardware that is using the /dev/3dfx device and you are NOT running 
as root, it seems that the restart doesn't clear some sort of permission. 
Running Q3A Demo as root will fix this problem.  


Q. Performance is so slow I can't play! The frame rate looks like one
frame per second!

A. Most of the current drivers for Linux do not support hardware
acceleration at any bit depth other than 15 or 16 bit color. Make sure
that you specify 16 bit color when you run your X11 Server, such as:

	startx -- -bpp 16


Q. I have a Voodoo3.  When I start the game, I send up at a low resolution
version of my desktop, or the screen is completely black (with a blue
mouse cursor).

A. The current Voodoo3 X11 Servers have problems with DGA Mouse support.
This is a known driver issue and will be fixed in newer revisions of the
Voodoo3 X11 Server.  Run the game with the following to disable it:

	./q3demo +set in_dgamouse 0


Q. I'm running Gnome with Englightenment and sound doesn't work.

A. If you are using the Enlightenment Sound Daemon (esd), it will not release
the sound hardware for Quake3's usage.  You can disable it before running
the demo as follows:

		esdctl off  # turn it off
		start q3demo the normal way
		esdctl on   # turn it back on


Q. I try to run Q3A, but sound doesn't work.  The console says "Can't open 
/dev/dsp.  /dev/dsp:  Can't allocate memory."

A. This can happen if the vmware kernel modules are loaded. Assuming you are 
using a RedHat system, you can unload them by typing when logged as root

	/etc/rc.d/init.d/vmware stop


Q. I was playing Q3A and suddenly it quit and reported a signal error.

A. This looks like a bug. Please report it to id Software (see below).



--------------------------
(6) CONTACTING ID SOFTWARE
--------------------------
If you encounter a bug in the Q3A Demo, particularly those involving
video or network bugs. please email a description of the bug to

  feedback@quake3arena.com 

Please do NOT send reports to individual id employees.

In your subject line, please describe the system the game is being played
on (Mac, Linux, Win32) and the type of problem you are reporting: video,
network, sound or game. Example Subject Line:  "Mac/video problem" or
"Linux/network connection problem."

In the body of your letter (no attached files please), briefly list and
describe the problems.  Detailed descriptions of problems are good, but
remember that brevity is best. Please do NOT send screen shots unless
they are the ONLY way to show a problem.

While we realize that you may have comments and suggestions regarding
specific game play features, please refrain from submitting such along
with bug reports. Comments on game play can be made on the official
Quake 3: Arena message board at:

    http://www.quake3arena.com/




------------------------------------------
(7) CONTACTING LOKI ENTERTAINMENT SOFTWARE
------------------------------------------
If you have problems with the installer, or want to submit
suggestions or corrections regarding the Linux version of
the HTML manual, please contact support@lokigames.com.

If you like the demo and consider purchasing the full version of
the game, you can order it from your local software reseller, or
buy online directly from Loki Entertainment Software at:

    http://www.lokigames.com/products/quake3/

or proceed directly to the order page at https://www.lokigames.com/.

Loki Entertainment Software has created a newsgroup devoted to
discussion of Linux Quake III Arena:

    news://news.lokigames.com/loki.games.quake3

The quake3.lokigames.com website will offer news and support for
Linux projects relating to Quake III Arena. 

Enjoy!
- The Loki Quake III Arena Team
