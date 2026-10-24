/* SPDX-License-Identifier: MIT
 * CiukiOS Help topics (English UI copy) and the topic model: lookup, index
 * and full-text search. The dedication text is the owner's approved 0.8
 * wording (src/apps/about.c on legacy-0.8). Program topics are keyed by the
 * program name in /Programs (the argument of du_help_topic). */
#include "help.h"
#include <stdlib.h>
#include <string.h>

const struct help_topic help_topics[] = {
{"welcome", "Welcome to CiukiOS", NULL,
 "welcome;CiukiOS;Ciuki;dedication;overview",
"Welcome to CiukiOS\n"
"\n"
"Welcome to CiukiOS - Classic computing, one woof at a time.\n"
"\n"
"CiukiOS is a retro gaming operating system for PCs of the late\n"
"1990s and early 2000s and for their emulated twins. It boots its\n"
"own 32-bit kernel, runs native programs on a POSIX-like interface,\n"
"and keeps DOS programs and games in virtual machines shown in\n"
"desktop windows.\n"
"\n"
"Choose a topic on the Contents tab, look a word up on the Index\n"
"tab, or search the whole of Help on the Search tab. Press F1 in\n"
"any program to see the topic about that program. Use File > Open\n"
"to read any text file.\n"
"\n"
"CiukiOS is dedicated to Ciuki, the dog of the boot screen and of\n"
"the logo.\n"},

/* ---- Using CiukiOS ---- */
{"start", "Getting started", "Using CiukiOS",
 "Start menu;windows;taskbar;close box;title bar;programs menu",
"Getting started\n"
"\n"
"1. Click Start at the left of the taskbar, or press the Windows\n"
"   key or Ctrl+Esc, to open the Start menu. It lists Programs,\n"
"   Documents, Settings, Help, Run and Shut Down.\n"
"2. Click a program to open it in a window. Each open window has a\n"
"   button on the taskbar; click it to bring the window to the\n"
"   front, or to minimize it again.\n"
"3. Drag a window by its title bar. The close box in the title bar\n"
"   closes it; programs with unsaved work ask first.\n"
"4. Files shows the disk: CiukiOS (/), Applications (/Programs),\n"
"   System (/system) and your folder (/Documents).\n"
"5. Click Start, then Shut Down, before switching the computer off.\n"},
{"desktop", "The desktop", "Using CiukiOS",
 "desktop;icons;wallpaper;taskbar;tray;notification area;Recycle Bin;Start menu;context menu",
"The desktop\n"
"\n"
"The desktop holds icons for Computer, your documents and the\n"
"Recycle Bin, and any shortcuts you add. Double-click an icon to\n"
"open it; right-click for its menu, or right-click the empty\n"
"desktop to arrange icons, refresh or change the display.\n"
"\n"
"The taskbar at the bottom has the Start button, one button for\n"
"each open window, and the notification area (the tray) with the\n"
"volume, network, battery and clock. Rest the pointer on a tray\n"
"icon to see what it shows. Click the speaker for a quick volume\n"
"slider with Mute; double-click the clock to set the date and time.\n"
"\n"
"Deleted files go to the Recycle Bin; open it to restore them or\n"
"to empty it. Choose Start > Settings > Control Panel to change\n"
"the wallpaper, colours, sounds and other settings.\n"
"\n"
"Press F1 with the desktop selected to open this topic.\n"},
{"keys", "Keyboard shortcuts", "Using CiukiOS",
 "shortcuts;keyboard;hotkeys;accelerator;F1;Alt+Tab;Alt+F4;Ctrl+C;Ctrl+V",
"Keyboard shortcuts\n"
"\n"
"In every window\n"
"  F1               Help for the program.\n"
"  Shift+F1         What's This?: click a control to explain it.\n"
"  Alt or F10       Open the menu bar; arrows move, Enter chooses.\n"
"  Alt+letter       Open that menu or press that button.\n"
"  Tab, Shift+Tab   Move between controls.\n"
"  Esc              Close a menu or a dialog.\n"
"  Ctrl+C/X/V       Copy, cut and paste text.\n"
"\n"
"On the desktop\n"
"  Win or Ctrl+Esc  Open the Start menu.\n"
"  Alt+Tab          Switch between windows.\n"
"  Alt+F4           Close the active window.\n"
"\n"
"Files      Enter open, F2 rename, Del delete, F5 refresh,\n"
"           Backspace up one level.\n"
"CiukNote   Ctrl+N new, Ctrl+O open, Ctrl+S save, Ctrl+F find,\n"
"           F3 find next, Ctrl+H replace, Ctrl+G go to, F5 date.\n"
"Tasks      Del ends the selected task or process.\n"
"Calculator Enter =, Esc C, Del CE, @ square root, r 1/x,\n"
"           F9 +/-, Ctrl+L/R/M/P memory.\n"
"Viewer     PgUp/PgDn previous/next picture, + and - zoom,\n"
"           F best fit, 1 actual size, arrows scroll.\n"
"Clock      Arrows move the day, PgUp/PgDn the month,\n"
"           Home today.\n"
"Volume     Up/Down and PgUp/PgDn volume, Left/Right balance,\n"
"           M mute.\n"},
{"help", "Using Help", "Using CiukiOS",
 "Help;contents;index;search;F1;What's This;tooltips;Back;topics",
"Using Help\n"
"\n"
"Open Help with F1 in any program (you see the topic about that\n"
"program), from Start > Help, or from a program's Help menu.\n"
"\n"
"Contents   Books hold the topics. Double-click a book, or press\n"
"           Right and Left, to open and close it; click a topic to\n"
"           read it.\n"
"Index      Type the first letters of a word; the list jumps to\n"
"           it. Press Enter or double-click to read the topic.\n"
"Search     Type words and click List Topics. A topic is listed\n"
"           when it contains every word; words match the start of\n"
"           longer words (print finds printing). Put several words\n"
"           in quotation marks to find them together.\n"
"Back       Returns to the topic you read before.\n"
"\n"
"Other help in every program\n"
"  Tooltips     Rest the pointer on a button; a yellow label\n"
"               names it.\n"
"  What's This? Press Shift+F1, or click the ? button in the title\n"
"               bar of a dialog, then click a control to read\n"
"               what it does.\n"
"File > Open shows any text file in this window, read-only.\n"},
{"disk", "Files and folders", "Using CiukiOS",
 "files;folders;disk;FAT32;long names;/Documents;/Programs;/system;Recycle Bin",
"Files and folders\n"
"\n"
"The system disk is FAT32. Long names are supported. Folders:\n"
"  /Programs   programs (each one appears in the Start menu)\n"
"  /Documents  your documents and the recent documents list\n"
"              (/Documents/recent.lst)\n"
"  /system     system files, licences and assets\n"
"  /Games      bundled shareware games disk\n"
"  /tmp        temporary files\n"
"\n"
"Open a file in Files with Enter or a double click: text files\n"
"open in CiukNote, pictures in the Image Viewer, programs run.\n"
"Deleting a file moves it to the Recycle Bin.\n"},
{"dos", "DOS and games", "Using CiukiOS",
 "DOS;games;virtual machine;CiukiDOS;Sound Blaster;full screen;dosbox",
"DOS and games\n"
"\n"
"The DOS window (dosbox) starts CiukiDOS in a virtual 8086 machine\n"
"shown in a desktop window, with a virtual VGA, keyboard, mouse,\n"
"Sound Blaster 16 and OPL3 music. Click the window to give it the\n"
"keyboard. Closing the window ends the machine. See the DOS\n"
"Window topic for the details.\n"
"\n"
"Commands inside CiukiDOS: DIR, CD, TYPE, COPY, DEL, REN, MD, RD,\n"
"VER, HELP. DOS games are not distributed with CiukiOS; copy your\n"
"own to the disk. Start > Programs > Games starts the bundled\n"
"shareware games.\n"},
{"sound", "Sound and volume", "Using CiukiOS",
 "sound;volume;mute;audio;AC'97;speakers;balance",
"Sound and volume\n"
"\n"
"CiukiOS plays sound through Intel AC'97 compatible audio. Volume\n"
"Control sets the master volume (0 to 100), the balance and mute;\n"
"the setting is saved in C:\\SYSTEM\\AUDIO.CFG and kept after a\n"
"restart. A muted system stays muted until you clear Mute. DOS\n"
"games use the virtual Sound Blaster; their own volume settings\n"
"never change the master volume.\n"
"\n"
"Control Panel > Sounds and Audio chooses the sound for each\n"
"event, such as the start-up chime, a warning or a new e-mail.\n"},

/* ---- Programs ---- */
{"files", "Files", "Programs",
 "Files;folders;copy;move;rename;delete;Recycle Bin;properties;view;address bar;icons;details;sort",
"Files\n"
"\n"
"Files shows the disks and folders of the computer. The folder\n"
"tree is on the left and the contents of the open folder on the\n"
"right.\n"
"\n"
"Open        Double-click, or select and press Enter. Text files\n"
"            open in CiukNote; File > Open with CiukNote forces it.\n"
"New         File > New Folder and New Text Document.\n"
"Rename      Select and press F2.\n"
"Delete      Select and press Del; the item goes to the Recycle\n"
"            Bin (Go > Recycle Bin restores or empties it).\n"
"Copy, move  Edit > Cut, Copy and Paste, or drag between folders;\n"
"            the pointer shows the effect.\n"
"Properties  Alt+Enter shows the size, dates and type.\n"
"\n"
"View        Large Icons, List and Details; sort by Name, Size,\n"
"            Type or Modified; F5 refreshes.\n"
"Go          Back (Alt+Left), Forward (Alt+Right), Up One Level\n"
"            (Alt+Up), the Address Bar (Alt+D) and the main\n"
"            places: Computer, Programs, Documents, Desktop,\n"
"            System, Games, Control Panel and the Recycle Bin.\n"},
{"ciuknote", "CiukNote", "Programs",
 "CiukNote;Notepad;text editor;find;replace;word wrap;font;go to;save;open",
"CiukNote\n"
"\n"
"CiukNote edits plain text files, like Notepad. Files may be up to\n"
"60 KB.\n"
"\n"
"File    New (Ctrl+N), Open (Ctrl+O), Save (Ctrl+S), Save As and\n"
"        Exit. A document with unsaved changes asks before it is\n"
"        replaced or closed.\n"
"Edit    Undo (Ctrl+Z), Cut, Copy, Paste, Delete, Find (Ctrl+F),\n"
"        Find Next (F3), Replace (Ctrl+H), Go To (Ctrl+G), Select\n"
"        All (Ctrl+A) and Time/Date (F5).\n"
"Format  Word Wrap and Font (regular or bold).\n"
"View    Status Bar, with the line and column.\n"
"\n"
"Double-click selects a word. Right-click for a menu of editing\n"
"commands.\n"},
{"wordpad", "WordPad", "Programs",
 "WordPad;word processor;rich text;RTF;document;write;doc;wri;editor;print;insert;object;picture",
"WordPad\n"
"\n"
"WordPad writes and edits documents with fonts, colours, bullets,\n"
"tabs and pictures. It opens Rich Text Format (.rtf), Word 6, 95\n"
"and 97 (.doc), Write (.wri) and text (.txt) files up to 16 MB and\n"
"saves Rich Text Format or text. A document opened from Word or\n"
"Write is saved under a new .rtf name.\n"
"\n"
"File      New, Open, Save, Save As, Print (Ctrl+P), Print Preview,\n"
"          Page Setup and Exit.\n"
"Edit      Undo and Redo (Ctrl+Z, Ctrl+Y) go back and forward over\n"
"          many changes. Cut, Copy and Paste keep the formatting\n"
"          between WordPad windows and other programs. Find,\n"
"          Find Next (F3) and Replace (Ctrl+H).\n"
"View      Toolbar, Format Bar, Ruler, Status Bar and Options.\n"
"Insert    Date and Time, Picture and Object.\n"
"Format    Font, Bullet Style, Paragraph and Tabs.\n"
"\n"
"Press F1 for this Help and Shift+F1, then click a button, to see\n"
"what it does.\n"
},
{"wordpad-formatting", "Formatting text in WordPad", "Programs",
 "format;font;bold;italic;underline;colour;paragraph;alignment;justify;bullets;indent;WordPad",
"Formatting text in WordPad\n"
"\n"
"Select the text, then choose Format > Font or use the format bar:\n"
"font, size, colour, Bold (Ctrl+B), Italic (Ctrl+I) and Underline\n"
"(Ctrl+U). With nothing selected the choice applies to the text\n"
"you type next.\n"
"\n"
"Format > Paragraph sets the left, right and first-line indents and\n"
"the alignment: Left (Ctrl+L), Center (Ctrl+E), Right (Ctrl+R) or\n"
"Justify (Ctrl+J, which spreads the words of every full line to\n"
"both margins). Format > Bullet Style starts a bulleted list;\n"
"press Enter on an empty bullet to end it.\n"
"\n"
"Format > Tabs sets the tab stops of the selected paragraphs.\n"
},
{"wordpad-ruler", "WordPad ruler, tabs and units", "Programs",
 "ruler;indent;tab stop;units;inches;centimeters;points;picas;word wrap;no wrap;wrap to ruler;options;WordPad",
"WordPad ruler, tabs and units\n"
"\n"
"The ruler shows the margins of the paragraph at the caret. Drag\n"
"the top marker to change the first-line indent, the lower left\n"
"marker to move the left indent and the right marker to change\n"
"the right indent. Click an empty place on the ruler to add a tab\n"
"stop, drag a tab stop to move it, or drag it off the ruler to\n"
"remove it. The change applies to the selected paragraphs.\n"
"\n"
"View > Options chooses the measurement units (inches,\n"
"centimeters, points or picas) for the ruler and the dialogs, and\n"
"the word wrap of each kind of document: to the window, no wrap\n"
"(lines run on, use the scroll bar) or to the ruler (the width of\n"
"the printed page).\n"
},
{"wordpad-pictures", "Pictures and objects in WordPad", "Programs",
 "picture;image;object;bitmap;BMP;PNG;JPEG;GIF;insert object;embedded;WordPad",
"Pictures and objects in WordPad\n"
"\n"
"Insert > Picture puts a BMP, PNG, JPEG or GIF file at the caret.\n"
"Insert > Object offers a new blank Bitmap Image or a picture\n"
"from a file. A picture is part of the document: it is kept when\n"
"you save as Rich Text Format, copied with the text, and printed.\n"
"A picture wider than the page is reduced to fit.\n"
"\n"
"Pictures and objects in documents from other programs are shown\n"
"when they are PNG, JPEG or bitmaps; other kinds are kept and\n"
"shown as a frame.\n"
},
{"wordpad-files", "Word and Write files in WordPad", "Programs",
 "Word;doc;wri;Write;import;convert;Windows 3.1;Word 6;Word 97;WordPad",
"Word and Write files in WordPad\n"
"\n"
"WordPad opens Word for Windows 6.0, Word 95 and Word 97 and\n"
"later documents (.doc) and Write documents (.wri) with their\n"
"fonts, sizes, bold, italic, underline, colours, alignment,\n"
"indents, tab stops, bullets and pictures. Tables become\n"
"tab-separated lines. Headers, footers, footnotes and text boxes\n"
"are not shown; WordPad says so when it opens such a file.\n"
"\n"
"Documents with a password, and files from versions of Word\n"
"before 6.0, cannot be opened. Save the document as Rich Text\n"
"Format or text in the program that made it.\n"
},
{"wordpad-printing", "Printing from WordPad", "Programs",
 "print;printer;preview;page setup;paper;margins;pages;WordPad",
"Printing from WordPad\n"
"\n"
"File > Print Preview shows the pages with the margins of File >\n"
"Page Setup. File > Print (Ctrl+P) or the Print button of the\n"
"preview opens the Print dialog: choose the printer, the pages and\n"
"the number of copies, or print to a file. PostScript and PCL\n"
"printers print the fonts, colours and pictures; a text printer\n"
"prints the characters. The job appears in Printers.\n"
},
{"ciukide", "CiukIDE", "Programs",
 "CiukIDE;IDE;compiler;build;run;project;C;program;developer",
"CiukIDE\n"
"\n"
"CiukIDE is a small development environment for C programs that\n"
"run on CiukiOS. It builds with the Tiny C Compiler.\n"
"\n"
"Project  New Project creates a Hello window or Hello console\n"
"         program in /Documents/Developer. Open Project (Ctrl+O)\n"
"         loads one, Save (Ctrl+S) stores the source, Build (F7)\n"
"         compiles it and Run (F5) starts it. Open project.cfg\n"
"         shows the project's settings in Files.\n"
"Edit     Undo, Cut, Copy, Paste and Select All.\n"
"\n"
"Build messages appear below the source. The Developer guide has\n"
"the programming interface and examples.\n"},
{"paint", "CiukPaint", "Programs",
 "CiukPaint;Paint;drawing;bitmap;pencil;brush;fill;text;select;colour;zoom;flip;rotate",
"CiukPaint\n"
"\n"
"CiukPaint draws and edits pictures (BMP and PNG files).\n"
"\n"
"Tool Box  Select, Eraser, Fill, Pick Colour, Magnifier, Pencil,\n"
"          Brush, Airbrush, Text, Line, Curve, Rectangle, Polygon,\n"
"          Ellipse and Rounded Rectangle. Rest the pointer on a\n"
"          tool to read what it does; options appear below the\n"
"          tools.\n"
"Colours   Click for the foreground colour, right-click for the\n"
"          background colour.\n"
"File      New (Ctrl+N), Open (Ctrl+O), Save (Ctrl+S), Save As.\n"
"Edit      Undo (Ctrl+Z), Repeat (Ctrl+Y), Cut, Copy, Paste,\n"
"          Clear Selection and Select All.\n"
"View      Tool Box (Ctrl+T), Colour Box (Ctrl+L), Status Bar,\n"
"          Zoom In and Out (Ctrl+PgDn and Ctrl+PgUp), 1x to 8x,\n"
"          Show Grid (Ctrl+G) and Text Font.\n"
"Image     Flip/Rotate (Ctrl+R), Invert Colours (Ctrl+I),\n"
"          Attributes (Ctrl+E) for the size, Clear Image and\n"
"          Draw Opaque.\n"},
{"tasks", "Task Manager", "Programs",
 "Task Manager;tasks;processes;End Task;End Process;Switch To;performance;CPU;memory;not responding",
"Task Manager\n"
"\n"
"Task Manager lists what is running. Open it from Start >\n"
"Settings, or press Ctrl+Alt+Del.\n"
"\n"
"Applications  The open windows and whether each one is running\n"
"              or not responding. End Task closes the selected\n"
"              window's program, Switch To brings it forward and\n"
"              New Task starts a program by name.\n"
"Processes     Every process with its PID, parent, threads,\n"
"              memory and CPU use. End Process stops the selected\n"
"              one (Del does the same).\n"
"Performance   CPU use and its history, memory, kernel memory and\n"
"              the totals of processes, threads and uptime.\n"
"\n"
"View > Update Speed chooses High, Normal, Low or Paused; Refresh\n"
"Now (F5) updates once. File > Shut Down closes the session.\n"},
{"control", "Control Panel", "Programs",
 "Control Panel;settings;display;mouse;keyboard;sounds;date;time;system;network;power;game controllers;add remove programs;drivers",
"Control Panel\n"
"\n"
"The Control Panel changes how the computer looks and behaves.\n"
"Open it from Start > Settings; its pages are also listed there.\n"
"\n"
"Display          Background (wallpaper and its position), Appear-\n"
"                 ance (colour scheme) and Settings (resolution).\n"
"Mouse, Keyboard  Pointer speed, double-click time, buttons, the\n"
"                 keyboard layout (US or Italian) and repeat.\n"
"Sounds and Audio Volume, and the sound for each event.\n"
"Date and Time    Set the clock, the time zone and the date.\n"
"System           General information, Hardware check, Logs and\n"
"                 the Drivers installed, with their resources.\n"
"Network          The adapters, addresses and connection state.\n"
"Power            Battery and power settings.\n"
"Storage          Disks and removable devices; Safely Remove.\n"
"Add/Remove Programs\n"
"                 Installed programs and games; Remove runs the\n"
"                 program's uninstaller.\n"
"Game controllers Joysticks and their calibration.\n"
"\n"
"Changes apply when you click OK or Apply.\n"},
{"calc", "Calculator", "Programs",
 "Calculator;calculate;memory;percent;square root;arithmetic",
"Calculator\n"
"\n"
"Calculator does standard arithmetic. Click the buttons or type\n"
"on the keyboard.\n"
"\n"
"  + - * /     Operators; Enter or = gives the result.\n"
"  %           Percentage; @ square root; r reciprocal (1/x).\n"
"  F9          Change sign.\n"
"  Esc         Clear (C); Del clears the entry (CE).\n"
"  Ctrl+L      Clear memory; Ctrl+R recall; Ctrl+M store;\n"
"              Ctrl+P add to memory.\n"
"\n"
"Copy (Ctrl+C) puts the display on the clipboard and Paste\n"
"(Ctrl+V) enters a number from it.\n"},
{"clock", "Clock", "Programs",
 "Clock;calendar;time;date;analog;digital;month",
"Clock\n"
"\n"
"Clock shows an analog and a digital clock and a month calendar.\n"
"\n"
"  Left, Right     Move the selected day.\n"
"  PgUp, PgDn      Show the previous or next month.\n"
"  Home            Go back to today.\n"
"\n"
"To change the time, date or time zone, use Control Panel > Date\n"
"and Time (also reached by double-clicking the clock in the\n"
"taskbar).\n"},
{"viewer", "Image Viewer", "Programs",
 "Image Viewer;pictures;PNG;JPEG;BMP;GIF;zoom;slideshow;best fit",
"Image Viewer\n"
"\n"
"Image Viewer shows PNG, JPEG, BMP and GIF pictures. Open a\n"
"picture from File > Open or from Files; the other pictures in\n"
"the same folder follow.\n"
"\n"
"  PgUp, PgDn      Previous and next picture.\n"
"  + and -         Zoom in and out.\n"
"  F               Fit the picture to the window.\n"
"  1               Actual size.\n"
"  Arrow keys      Scroll a picture larger than the window.\n"},
{"mixer", "Volume Control", "Programs",
 "Volume Control;volume;balance;mute;speakers",
"Volume Control\n"
"\n"
"Volume Control sets the master volume, the left-right balance\n"
"and mute. The speaker in the notification area offers a quick\n"
"volume slider with Mute.\n"
"\n"
"  Up, Down, PgUp, PgDn   Volume.\n"
"  Left, Right            Balance.\n"
"  M                      Mute or unmute.\n"
"\n"
"The settings are kept after a restart. See Sound and volume.\n"},
{"charmap", "Character Map", "Programs",
 "Character Map;characters;symbols;copy;font",
"Character Map\n"
"\n"
"Character Map shows the characters of the system font. Click a\n"
"character to select it; click it again, or choose Select, to add\n"
"it to the Characters to copy box. Copy puts that text on the\n"
"clipboard to paste into another program.\n"},
{"music", "Music Player", "Programs",
 "Music Player;music;WAV;MP3;Ogg;playlist;play;pause;stop",
"Music Player\n"
"\n"
"Music Player plays WAV, MP3 and Ogg Vorbis files and M3U\n"
"playlists.\n"
"\n"
"File      Open a file, playlist or folder; Add Files or Add\n"
"          Folder to the playlist; Save Playlist (Ctrl+S); Clear\n"
"          Playlist.\n"
"Playback  Play, Pause, Stop, Previous (PgUp) and Next (PgDn).\n"
"\n"
"Double-click an entry of the playlist to play it. Playback\n"
"continues through the list. The master volume is set in Volume\n"
"Control.\n"},
{"regedit", "Registry Editor", "Programs",
 "Registry Editor;registry;key;value;string;integer;boolean;binary;import;export;find",
"Registry Editor\n"
"\n"
"Registry Editor shows and changes the registry, the settings\n"
"store of the system and its programs. Wrong changes can stop\n"
"programs from working: export a branch before editing it.\n"
"\n"
"The tree on the left lists the roots: System, Desktop, Programs,\n"
"Users and Hardware. The values of the selected key are on the\n"
"right.\n"
"\n"
"File  Import merges a registry file, Export branch saves the\n"
"      selected key and its subkeys, Refresh (F5) rereads.\n"
"Edit  New key, string, integer, boolean or binary value; Edit\n"
"      value; Delete; Find (Ctrl+F) and Find next (F3).\n"},
{"hwreport", "Hardware Report", "Programs",
 "Hardware Report;hardware;report;devices;drivers;PCI;save;rescue",
"Hardware Report\n"
"\n"
"Hardware Report collects what CiukiOS found in the computer: the\n"
"processor, memory, disks, display, network, audio and USB\n"
"devices, and which driver owns each. Use it to describe a\n"
"problem or to check that a machine is supported.\n"
"\n"
"Save writes the report as a text file; the file can be read in\n"
"CiukNote or in this Help window.\n"},
{"terminal", "Terminal", "Programs",
 "Terminal;command prompt;commands;DIR;CD;COPY;DEL;TYPE;Lua;shell;batch",
"Terminal\n"
"\n"
"Terminal is a command prompt in a window.\n"
"\n"
"DIR/ls [path]          CD/cd [path|Computer]\n"
"COPY/cp src dst        MOVE/mv src dst     REN src dst\n"
"DEL/rm files           MD/mkdir dir        RD/rmdir dir\n"
"TYPE/cat files         CLS/clear           ECHO text\n"
"VER  HELP  EXIT        START program [file]\n"
"NET                    AUDIO [volume 0..100|mute|unmute]\n"
"TASKS [KILL pid]       DISPLAY             MEM\n"
"DATE  TIME             SHUTDOWN            REBOOT\n"
"LUA [script.lua args]  script.lua [args]\n"
"\n"
"Quote paths with spaces; * and ? match names. Tab completes a\n"
"path, Up and Down recall the history, PgUp and PgDn scroll.\n"
"Ctrl+C copies the selection or interrupts a command; Ctrl+V\n"
"pastes. Right-click for Copy, Clear and Help.\n"},
{"gfxdemo", "Graphics Demo", "Programs",
 "Graphics Demo;graphics;benchmark;acceleration;display;test",
"Graphics Demo\n"
"\n"
"Graphics Demo draws animated scenes and measures the display:\n"
"a screen-size fill, a 256 x 256 copy and a screen-size 8 x 8\n"
"pattern. It shows whether the display driver accelerates\n"
"drawing. Close the window to stop it.\n"},
{"dosbox", "DOS Window", "Programs",
 "DOS Window;dosbox;DOS;CiukiDOS;virtual machine;games;keyboard;mouse;full screen",
"DOS Window\n"
"\n"
"The DOS window runs one CiukiDOS virtual machine. Click inside\n"
"it to give it the keyboard and mouse; press Alt+Tab or click\n"
"another window to leave. F1 and the other keys go to the DOS\n"
"program, not to Help, while the window has the focus.\n"
"\n"
"Closing the window ends the machine; the window asks first when\n"
"a program is running. Programs > Games opens the window and\n"
"starts a bundled game; to start your own, type its name at the\n"
"DOS prompt.\n"},
{"lua", "Lua", "Programs",
 "Lua;script;scripting;interpreter;.lua",
"Lua\n"
"\n"
"The Lua 5.4 interpreter runs scripts. Start it from Terminal:\n"
"\n"
"  LUA script.lua args    runs a script\n"
"  LUA                    interactive mode; .exit leaves it\n"
"\n"
"A file ending in .lua can also be run by its name.\n"},
{"netsurf", "NetSurf", "Programs",
 "NetSurf;browser;web;internet;address;links;pages;download",
"NetSurf\n"
"\n"
"NetSurf is the web browser. Type an address in the address bar\n"
"and press Enter; click a link to follow it. The Back and Forward\n"
"buttons move through the pages you have seen. The network\n"
"adapter must be connected: see Control Panel > Network.\n"},
{"demo", "Ciuki Demo", "Programs",
 "Ciuki Demo;demo;portrait;graphics",
"Ciuki Demo\n"
"\n"
"Ciuki Demo is a small window that shows the Ciuki portrait. It\n"
"also checks that the desktop and the graphics path work.\n"},

/* ---- Troubleshooting ---- */
{"troubleshooting", "Troubleshooting", "Troubleshooting",
 "troubleshooting;problems;trouble;repair;Safe mode",
"Troubleshooting\n"
"\n"
"Choose the problem that matches yours:\n"
"  A program is not responding\n"
"  There is no sound\n"
"  The screen looks wrong\n"
"  A DOS game will not start\n"
"  The network does not work\n"
"  Files or the disk give errors\n"
"\n"
"Safe mode: choose it in the boot menu to start without drivers\n"
"that failed before.\n"},
{"hung", "A program is not responding", "Troubleshooting",
 "not responding;hung;frozen;End Task;Task Manager;pointer;keyboard",
"A program is not responding\n"
"\n"
"1. Wait a few seconds: a program busy with a long job may\n"
"   answer in a moment.\n"
"2. Open Task Manager (Ctrl+Alt+Del, or Start > Settings), choose\n"
"   the program on the Applications tab and click End Task.\n"
"   Unsaved work in that program is lost.\n"
"3. If the pointer or the keyboard stops, press and release Alt,\n"
"   then click the desktop; the desktop repairs held keys and\n"
"   buttons.\n"
"4. If the whole desktop does not answer, restart the computer\n"
"   and, if it happens again, start in Safe mode.\n"},
{"nosound", "There is no sound", "Troubleshooting",
 "no sound;silent;mute;volume;speakers;audio;AC'97",
"There is no sound\n"
"\n"
"1. Check that the speakers are on, plugged in and turned up.\n"
"2. Open Volume Control and make sure Mute is clear and the\n"
"   volume is above zero.\n"
"3. Open Control Panel > Sounds and Audio and play a sound from\n"
"   the list; the Hardware Report shows whether an audio device\n"
"   was found.\n"
"4. DOS games use their own sound settings: choose Sound Blaster\n"
"   16 in the game's setup.\n"},
{"display-trouble", "The screen looks wrong", "Troubleshooting",
 "display;screen;resolution;colours;flicker;black screen;monitor",
"The screen looks wrong\n"
"\n"
"1. Open Control Panel > Display > Settings and choose a\n"
"   resolution that the monitor supports; CiukiOS asks you to\n"
"   confirm within 15 seconds or goes back to the old mode.\n"
"2. If the picture is unreadable, restart and choose Safe mode\n"
"   in the boot menu, then choose the resolution again.\n"
"3. Wrong colours or a garbled wallpaper: choose another one on\n"
"   the Background page.\n"},
{"dosgame-trouble", "A DOS game will not start", "Troubleshooting",
 "DOS game;game;will not start;memory;sound;crash;dosbox",
"A DOS game will not start\n"
"\n"
"1. Check the file name at the DOS prompt: DIR shows the files\n"
"   of the folder, then type the program's name.\n"
"2. Set the game's sound to Sound Blaster 16 (port 220, IRQ 7,\n"
"   DMA 1) and its music to OPL3 / Adlib.\n"
"3. Close other windows first: the DOS window needs memory.\n"
"4. Games are not included with CiukiOS except the shareware\n"
"   games in Programs > Games.\n"},
{"network-trouble", "The network does not work", "Troubleshooting",
 "network;Internet;adapter;address;DHCP;cable;browser",
"The network does not work\n"
"\n"
"1. Check that the network cable is plugged in at both ends.\n"
"2. Open Control Panel > Network: the adapter must be listed\n"
"   and connected, with an address. The NET command in Terminal\n"
"   shows the same.\n"
"3. If no adapter is listed, the Hardware Report shows whether\n"
"   the network device was found and which driver owns it.\n"},
{"disk-trouble", "Files or the disk give errors", "Troubleshooting",
 "disk;errors;file;cannot be read;scandisk;FAT32;full",
"Files or the disk give errors\n"
"\n"
"1. A file that cannot be read may be on a removable disk that\n"
"   has been taken out: put it back, or copy the file first.\n"
"2. Remove USB sticks and cards with Safely Remove (Files or\n"
"   Control Panel > Storage) so that nothing is lost.\n"
"3. A full disk refuses new files: empty the Recycle Bin and\n"
"   delete files you no longer need.\n"
"4. Always shut down with Start > Shut Down, so that everything\n"
"   is written to the disk first.\n"},

/* ---- Reference ---- */
{"about", "About CiukiOS", "Reference",
 "About;version;copyright;Ciuki;licence",
"About CiukiOS\n"
"\n"
"CiukiOS - Classic computing, one woof at a time.\n"
"Version 0.9.2 pre-alpha\n"
"\n"
"Dedicated to Ciuki\n"
"\n"
"In loving memory of Ciuk (Ciuki), the beloved dog behind\n"
"CiukiOS. Though you are no longer here, your memory lives on\n"
"every time this system starts.\n"
"\n"
"Copyright Alcybercloud.it and the CiukiOS contributors.\n"
"The kernel is GPL-2.0-only; desktop programs and libdeskui\n"
"are MIT.\n"},
{"credits", "Credits and licences", "Reference",
 "credits;licences;licenses;Tango;stb_image;Lua;newlib;copyright",
"Credits and licences\n"
"\n"
"Tango Icon Theme 0.8.90         Public Domain (assets/icons)\n"
"stb_image v2.30, Sean Barrett   MIT or Public Domain\n"
"                                (/system/licenses/stb_image.txt)\n"
"Lua 5.4.8                       MIT (/system/licenses)\n"
"newlib                          BSD-style (/system/licenses)\n"
"CiukiDOS                        FreeDOS components, GPL\n"
"\n"
"The Ciuki portrait and photograph are project assets of\n"
"CiukiOS; the code licences do not cover them.\n"},
};
const unsigned help_topic_count = sizeof(help_topics) / sizeof(help_topics[0]);

/* ---- the topic model ---- */
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int alnum(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int prefix_nocase(const char *s, const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) if (!s[i] || lower((unsigned char)s[i]) != lower((unsigned char)p[i])) return 0;
    return 1;
}
static int compare_nocase(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = lower((unsigned char)*a), y = lower((unsigned char)*b);
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}

int help_topic_find(const char *id)
{
    if (!id) return -1;
    for (unsigned i = 0; i < help_topic_count; i++) if (!strcmp(help_topics[i].id, id)) return (int)i;
    return -1;
}
int help_topic_argument(const char *argument)
{
    if (!argument || strncmp(argument, "topic:", 6)) return -1;
    return help_topic_find(argument + 6);
}

unsigned help_book_count(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < help_topic_count; i++) {
        const char *book = help_topics[i].book; unsigned seen = 0;
        if (!book) continue;
        for (unsigned j = 0; j < i; j++) if (help_topics[j].book && !strcmp(help_topics[j].book, book)) { seen = 1; break; }
        n += !seen;
    }
    return n;
}
const char *help_book(unsigned book)
{
    unsigned n = 0;
    for (unsigned i = 0; i < help_topic_count; i++) {
        const char *name = help_topics[i].book; unsigned seen = 0;
        if (!name) continue;
        for (unsigned j = 0; j < i; j++) if (help_topics[j].book && !strcmp(help_topics[j].book, name)) { seen = 1; break; }
        if (!seen && n++ == book) return name;
    }
    return NULL;
}
int help_topic_book(unsigned topic)
{
    if (topic >= help_topic_count || !help_topics[topic].book) return -1;
    for (unsigned b = 0; b < help_book_count(); b++) if (!strcmp(help_book(b), help_topics[topic].book)) return (int)b;
    return -1;
}

static int index_order(const void *a, const void *b)
{
    const struct help_index_entry *x = a, *y = b;
    int c = compare_nocase(x->keyword, y->keyword);
    return c ? c : x->topic < y->topic ? -1 : x->topic > y->topic;
}
unsigned help_index_build(struct help_index_entry *out, unsigned capacity)
{
    unsigned n = 0;
    for (unsigned t = 0; t < help_topic_count; t++) {
        const char *p = help_topics[t].keywords;
        while (p && *p) {
            const char *end = strchr(p, ';');
            size_t len = end ? (size_t)(end - p) : strlen(p);
            while (len && *p == ' ') { p++; len--; }
            while (len && p[len - 1] == ' ') len--;
            if (len && len < HELP_KEYWORD_MAX && n < capacity) {
                memcpy(out[n].keyword, p, len); out[n].keyword[len] = 0; out[n].topic = t; n++;
            }
            p = end ? end + 1 : p + strlen(p);
        }
    }
    qsort(out, n, sizeof(*out), index_order);
    return n;
}
unsigned help_index_seek(const struct help_index_entry *e, unsigned count, const char *prefix)
{
    size_t n = strlen(prefix);
    while (n && prefix[n - 1] == ' ') n--;
    if (!n) return 0 < count ? 0 : count;
    for (unsigned i = 0; i < count; i++) if (prefix_nocase(e[i].keyword, prefix, n)) return i;
    return count;
}

/* Occurrences of the word or phrase w (length wl) starting at a word boundary of hay. */
static unsigned count_word(const char *hay, const char *w, size_t wl)
{
    unsigned n = 0;
    for (size_t i = 0; hay[i]; i++) {
        if (i && alnum((unsigned char)hay[i - 1]) && alnum((unsigned char)w[0])) continue;
        if (prefix_nocase(hay + i, w, wl)) n++;
    }
    return n;
}
unsigned help_search(const char *query, struct help_hit *hits, unsigned capacity)
{
    char words[8][64]; size_t lengths[8]; unsigned count = 0;
    const char *p = query ? query : "";
    while (*p && count < 8) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *start = p; int quoted = *p == '"';
        if (quoted) { start = ++p; while (*p && *p != '"') p++; }
        else while (*p && *p != ' ' && *p != '\t') p++;
        size_t len = (size_t)(p - start);
        if (quoted && *p == '"') p++;
        if (!len) continue;
        if (len > 63) len = 63;
        memcpy(words[count], start, len); words[count][len] = 0; lengths[count++] = len;
    }
    if (!count || !capacity) return 0;
    unsigned found = 0;
    for (unsigned t = 0; t < help_topic_count; t++) {
        unsigned score = 0; int all = 1;
        for (unsigned w = 0; w < count && all; w++) {
            unsigned title = count_word(help_topics[t].title, words[w], lengths[w]);
            unsigned keywords = count_word(help_topics[t].keywords, words[w], lengths[w]);
            unsigned text = count_word(help_topics[t].text, words[w], lengths[w]);
            if (!title && !keywords && !text) all = 0;
            score += title * 8 + keywords * 6 + (text < 10 ? text : 10);
        }
        if (!all) continue;
        /* Insertion by score (stable: topic order breaks ties). */
        unsigned at = found;
        while (at && hits[at - 1].score < score) at--;
        if (at >= capacity) continue;
        if (found < capacity) found++;
        for (unsigned k = found - 1; k > at; k--) hits[k] = hits[k - 1];
        hits[at] = (struct help_hit){t, score};
    }
    return found;
}
