Aurora68 test disk, @DATE@

System 7.1 with every program built so far: Aurora68 @AURORA68@, Riptide @RIPTIDE@, Lagoon @LAGOON@, Octavo (Quill) @QUILL@. The programs that can write to the Pi's kernel log do so: open PiLog (it also starts at boot) to read it.

System Folder
- Extensions: PiDisk, PiMach, Wi-Fi & Bluetooth (the two menu-bar menus), Riptide (TCP/IP), Lagoon EtherTalk (AppleTalk over Ethernet).
- Riptide DNR (names for MacTCP programs). MacTCP is in "Control Panels (disabled)".
- Startup Items: PiLog.

Top level of the disk: PiBlue, PiNet, PiPing, PiStatus, PiWiFi.

Utilities: PiLog, PiPing, PiColor, PiScreens, PiDepth (rename it "PiDepth 1", "PiDepth 16"... to set that depth), Riptide Command Terminal, RiptidePrefs, MacTCP test, Browser test.

Apps
- Octavo: the word processor (the Mac edition of Quill). Its Fonts folder must stay beside it. Sample, Charts, Pictures, Tables and Headers are test documents.
- Lagoon: network shares (SMB, NFS, AFP over TCP). Riptide must be running. "Put on Desktop" shows a share as an AppleShare volume. That needs Lagoon EtherTalk chosen in the Network control panel (Lagoon / Slot #9).

TCP/IP (Riptide): both network functions of the Aurora68 card, the wired port (en0, preferred while its cable is in) and the WiFi (en1, used when the wired link is down), DHCP on each; host name MacSE (MacSE.local on the network), name server 1.1.1.1 (asked first; the ones the DHCP servers name are added after it). RiptidePrefs in Utilities changes these; they are in System Folder:Preferences:Riptide:riptide.conf. Internet Explorer and other MacTCP programs use Riptide (MacTCP itself stays disabled).

AppleTalk over Ethernet: open the Network control panel and choose "Lagoon". The Chooser then finds AppleShare servers on the network.

Known problems
- Lagoon's bridge: one share at a time, guest login only from Lagoon's window, volumes show at most 2 GB.
