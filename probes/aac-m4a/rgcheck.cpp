// rgcheck.cpp — scope de-risk for the M4aEncoder plan: does an MP4 ReplayGain
// freeform atom read back through the SAME generic TagLib PropertyMap path that
// LocalFileSource uses (tryRG -> f.properties()["REPLAYGAIN_TRACK_GAIN"])?
// If yes, rip/rec RG on .m4a needs NO decode-side change. Throwaway.
#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>
#include <taglib/mp4item.h>
#include <taglib/tpropertymap.h>
#include <taglib/tstring.h>
#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: rgcheck <file.m4a>\n"); return 2; }
    const char* path = argv[1];

    // Write REPLAYGAIN_* as iTunes freeform atoms (the plain dialect the plan
    // proposes for tagFile's is_m4a branch).
    {
        TagLib::MP4::File f(path, false);
        if (!f.isValid() || !f.tag()) { std::printf("open-for-write failed\n"); return 1; }
        f.tag()->setItem("----:com.apple.iTunes:REPLAYGAIN_TRACK_GAIN",
                         TagLib::MP4::Item(TagLib::StringList("-6.92 dB")));
        f.tag()->setItem("----:com.apple.iTunes:REPLAYGAIN_ALBUM_GAIN",
                         TagLib::MP4::Item(TagLib::StringList("-7.10 dB")));
        if (!f.save()) { std::printf("save failed\n"); return 1; }
    }

    // Read back exactly as LocalFileSource does: generic PropertyMap.
    {
        TagLib::MP4::File f(path, true);
        TagLib::PropertyMap props = f.properties();
        auto show = [&](const char* key) {
            if (props.contains(key) && !props[key].isEmpty())
                std::printf("  PropertyMap[%s] = '%s'\n", key, props[key].front().to8Bit(true).c_str());
            else
                std::printf("  PropertyMap[%s] = <ABSENT>\n", key);
        };
        std::printf("read-back via generic PropertyMap (LocalFileSource path):\n");
        show("REPLAYGAIN_TRACK_GAIN");
        show("REPLAYGAIN_ALBUM_GAIN");
    }
    return 0;
}
