# CiukiOS 0.8.0 development screenshots

These PNGs are direct QEMU framebuffer captures from 2026-09-30 and 2026-10-01. They are not
physical-hardware evidence. No image content has been edited.

| Image | QEMU capture | What it shows |
| --- | --- | --- |
| `desktop.png` | `build/tests/ui-refresh-verified-capture/` | Unified system readings in the top bar and Ciuki identity, 2026-10-01. |
| `about.png` | `build/tests/native-gl-about/about-gl.ppm` | About with version, component credits including TinyGL, GPL notice, Alcybercloud.it attribution and approved dedication. The PNG is a lossless conversion of the QEMU capture. |
| `ciuki-menu.png` | `build/tests/ui-refresh-verified-capture/` | The top-left Ciuki portrait and name open the system menu, 2026-10-01. |
| `network-settings.png` | `build/tests/network-advanced-final-20261001/dhcp/` | IPv4 and advanced settings with a detected QEMU NE2000 PCI adapter after a DHCP lease, 2026-10-01. |
| `network-adapter.png` | `build/tests/network-advanced-final-20261001/static/` | The Network applet opens Device Manager with the detected PCI adapter selected, 2026-10-01. |
| `network-drivers.png` | `build/tests/network-advanced-final-20261001/dhcp/` | NE2000 disabled in the Drivers view; the QEMU reboot then omits its load, 2026-10-01. |
| `first-web.png` | `build/tests/qemu-record-web-20261001/web-final/browser-page.png` | Historical MicroWeb HTTP proof before CiukWeb became the default. PNG copied byte-for-byte from the test capture. |
| `ciukweb.png` | `build/tests/native-gl-final2-ciukweb/ciukweb-page.ppm` | CiukWeb renders the same public HTTP page in a native desktop module. The PNG is a lossless format conversion of the QEMU framebuffer. |
| `opengl-triangle.png` | `build/tests/native-gl-final2-opengl/opengl-triangle.ppm` | The TinyGL software OpenGL subset draws a coloured triangle inside an M4 DOS window. |
| `ciukpaint-live-stroke.png` | `build/tests/native-gl-final2-paint/stroke-held.ppm` | CiukPaint's pencil mark is visible while the mouse button remains pressed. |
| `files.png` | `build/tests/desktop-polish-080-final-20260930/first/` | Files with its independent Properties window on the 0.8.0 image. |
| `desktop-menu.png` | `build/tests/desktop-polish-080-final-20260930/first/` | Desktop context menu and hover state on the 0.8.0 image. |
| `long-names.png` | `build/tests/long-names-080-final-20260930/` | Long FAT16 names in Files; the long-name gate passed on the final 0.8.0 image. |
| `ciukpaint.png` | `build/tests/ciukpaint-080-final-20260930/` | Original CiukPaint tools and a test drawing; its gate passed on the final 0.8.0 image. |
| `task-manager.png` | `build/tests/m4-tasks-showcase-final-20260930/` | Final-image Task Manager Virtual Machines tab, with the DOS guest selected and Give Focus / End VM controls. |
| `device-manager.png` | `build/tests/desktop-polish-080-final-20260930/first/` | Detected QEMU devices with a user-installed system font; no physical-PC claim. |
| `doom-window-prototype.png` | `build/tests/m4-gate-kbdqueue-20260930/` | Earlier M4 development capture, retained for the debugging history. |
| `doom-window.png` | `build/tests/vm-window-profile-080-final-20260930/doom-original/` | DOOM gameplay while Files is open, from the passing 0.8.0 profile. |
| `two-dos-vms.png` | `build/tests/vm-window-profile-080-final-20260930/doom-original/` | Two separate text DOS VMs before the gate's click-focus and independent-close checks. |
| `testgames-folder.png` | `build/tests/testgames-final-folder/` | Final TestGames folder in Files with the three distinct DOS game launchers. |
| `doom-from-testgames.png` | `build/tests/testgames-final-folder/` | DOOM launched by Files from the final TestGames folder into an M4 DOS window. |
| `settings-registry.png` | `build/tests/registry-final-20260930/read-delete/` | Ciuki Settings Registry showing a value saved on a previous QEMU boot. The public registry gate passed on the rebuilt image. |
| `win32-status.png` | `build/tests/win32-status-three-games/` | Free PE32 probe recognized without forking on the final three-game image; Win32 execution is not implemented. |

The README gallery captures come from separate QEMU development builds of
0.8.0. `doom-window-prototype.png` is retained separately as an earlier M4
development image. The 2026-09-30 VM-window profile passed 26/26 gates.
