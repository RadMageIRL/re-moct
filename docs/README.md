# RE-MOCT - Music On Console Terminal

> A homage to MOC - keyboard-driven terminal CD player and ripper for Windows.

📖 **[Documentation & Feature Guide](https://radmageirl.github.io/re-moct/)**

---

**Status:** Work in progress - no release yet. Polishing the Windows client, Linux port planned.

Built with C++20 · ncurses · miniaudio · TagLib · AccurateRip

📧 tattersgaming@gmail.com


Example TUI:

<img width="1367" height="664" alt="image" src="https://github.com/user-attachments/assets/d0d189b7-d301-4ae7-a2ac-dd62a507cbb0" />
<br><br>
<img width="1125" height="618" alt="image" src="https://github.com/user-attachments/assets/eccefb9e-1d83-4422-b72a-113b282a68d7" />
<br><br>
<img width="1124" height="616" alt="image" src="https://github.com/user-attachments/assets/0b67803d-2800-4e4b-aec4-697e0e807744" />
<br><br>
<img width="703" height="795" alt="image" src="https://github.com/user-attachments/assets/00ca1e84-b779-4bb9-b47e-f8cf5fea6c19" />

## Acknowledgements

- **[Leo Bogert](https://github.com/leo-bogert/accuraterip-checksum)** - authoritative AccurateRip CRCv2 reference implementation (`accuraterip-checksum.c`), which confirmed the correct formula including disc-absolute pregap anchoring
- **[AccurateRip](https://www.accuraterip.com)** (Spoon) - the AccurateRip verification database, used under non-commercial terms. Drive offset database sourced from [accuraterip.com/driveoffsets.htm](https://www.accuraterip.com/driveoffsets.htm)
- **[MusicBrainz](https://musicbrainz.org)** - open music metadata database, DiscID lookup and text search
- **[whipper](https://github.com/whipper-team/whipper)** & **[CUETools](http://cue.tools/wiki/CUETools)** - reference implementations consulted during AccurateRip research
