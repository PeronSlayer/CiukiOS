======================================
============ Introduction ============
======================================

   This is the DOS 6.22 device driver for RealTek RTL8101E and 
   RTL8102E(L), the Fast Ethernet controller with PCI-Express interface.



===================================
============ Copyright ============
===================================


   The driver is based on the RTL8139(A/B) Programming Guide (v0.1).
   This packet driver is designed as a generic Realtek 8101/8136/8139 
   ethernet controller. In no way is it supported by Realtek company 
   so use it at your risk.
   2009(c)  je, j_lestrada@yahoo.com

   Copyright, 1988-1992, Russell Nelson, Crynwr Software

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 1.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.


=======================================
============ Requirements  ============
=======================================

   - TASM – Turbo assembler 4.1
   - TLINK – Turbo link version 7.1.30.1
   - exe2com.exe  Converts an EXE file to flat COM file.
   - DJGPP v2.++  (32-bit extender)
   - WATTCP32

===========================================
============ Location of files ============
===========================================

   Location of Driver:          \RTLPKT\RTLPKT.COM

   Location of Source files:    \RTLPKT\SRC 

   Location of DOCS:            \RTLPKT\DOC


====================================================
============ Sample Configuration file  ============
====================================================

   AUTOEXEC.BAT
      C:\RTLPKT\RTLPKT 0x60



===================================
============ Usage  ============
===================================



     1.  Load the packet driver using the software interrupt and any
         optional switches if required.

         Usage: RTLPKT [options] <packet_int_no> 

         Example: RTLPKT -i 0x7e

         The only required parameter is the software interrupt.  A packet
         driver needs to have a software interrupt assigned to it for other
         programs to access it. The recommended interrupt is 0x7e. If you need
         any of the  options  listed below,  place them before the software
         interrupt on the command line.

         -d - Delayed initialization.  Used for diskless booting
         -n - NetWare conversion.  Converts 802.3 packets into 8137 packets
         -w - Windows hack, obsolete by winpkt
         -p - Promiscuous mode disable
         -u - Uninstall


      2. Type command C:\RTSPKT 0x60, the screen will appear following
         message:

         System: [345]86 processor, PCI bus, Two 8259s
         Packet driver software interrupt is 0x60
         Interrupt number is 0x9
         I/O port is 0x6100
         My Ethernet address is 52:54:4C:29:29:AD 

        This indicates that the driver successfully loaded into memory and 
        is ready.


===================================
============ Compiling ============
===================================



    1. Enter the /SRC folder, this will be in the same directory where 
       the main file was unpacked. 
    2. I had trouble with the “makefile” of DJGPP so I  compile by using batch 
       files. Here is the whole process:


       tasm /zi head.asm
       tasm /zi skeleton.asm
       tasm /zi tail.asm

       tlink head skeleton tail

       exe2com head.exe

       rename head.com RTLPKT.COM

    3. To test the packet driver I use the ping.exe that is part of
       WATTCP32 package. 


======================================
============ Contact info ============
======================================

   The latest version of Allegro can always be found on the Allegro 
   homepage, http://rtl8101packetdr.sourceforge.net. 




