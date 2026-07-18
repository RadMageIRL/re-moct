// ArtEmbed.cpp - see ArtEmbed.h. Per-format embedded-art read/write via TagLib.
// The writer shapes mirror the existing inline rip/recorder blocks (FrontCover,
// description "Cover"), fed the blob's own MIME.
#include "ArtEmbed.h"

#include "StringUtils.h"   // utf8_to_wide

#include <taglib/attachedpictureframe.h>
#include <taglib/apefile.h>
#include <taglib/apetag.h>
#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#include <taglib/id3v2tag.h>
#include <taglib/mp4coverart.h>
#include <taglib/mp4file.h>
#include <taglib/mp4item.h>
#include <taglib/mp4tag.h>
#include <taglib/mpegfile.h>
#include <taglib/opusfile.h>
#include <taglib/tbytevector.h>
#include <taglib/tlist.h>
#include <taglib/tstring.h>
#include <taglib/wavpackfile.h>
#include <taglib/xiphcomment.h>

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

#ifdef _WIN32
#define TL_PATH(p) utf8_to_wide(p).c_str()
#else
#define TL_PATH(p) (p).c_str()
#endif

namespace {

std::string lowerExt(const std::string& path) {
    std::string e = fs::path(path).extension().string();
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    return e;
}

// JPEG/PNG magic -> MIME; default jpeg (the APE binary item carries no MIME, so
// this is the WavPack read fallback and a defensive default for a bare blob).
std::string magicMime(const std::vector<uint8_t>& b) {
    if (b.size() > 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return "image/jpeg";
    if (b.size() > 8 && b[0] == 0x89 && b[1] == 0x50 && b[2] == 0x4E && b[3] == 0x47) return "image/png";
    return "image/jpeg";
}

std::optional<ArtBlob> blobFrom(const TagLib::ByteVector& data, const std::string& mime) {
    if (data.isEmpty()) return std::nullopt;
    std::vector<uint8_t> bytes(data.begin(), data.end());
    std::string m = mime.empty() ? magicMime(bytes) : mime;
    return ArtBlob{ std::move(bytes), m };
}

// FLAC / Opus share the FLAC::Picture list: front cover preferred, else first.
std::optional<ArtBlob> fromPictureList(const TagLib::List<TagLib::FLAC::Picture*>& pics) {
    if (pics.isEmpty()) return std::nullopt;
    const TagLib::FLAC::Picture* best = pics.front();
    for (const auto* p : pics)
        if (p && p->type() == TagLib::FLAC::Picture::FrontCover) { best = p; break; }
    if (!best) return std::nullopt;
    return blobFrom(best->data(), best->mimeType().to8Bit(true));
}

} // namespace

std::optional<ArtBlob> extractEmbeddedArt(const std::string& path) {
    const std::string ext = lowerExt(path);
    try {
        if (ext == ".flac") {
            TagLib::FLAC::File f(TL_PATH(path), false);
            if (!f.isValid()) return std::nullopt;
            return fromPictureList(f.pictureList());
        }
        if (ext == ".mp3") {
            TagLib::MPEG::File f(TL_PATH(path), false);
            if (!f.isValid() || !f.ID3v2Tag()) return std::nullopt;
            TagLib::ID3v2::AttachedPictureFrame* best = nullptr;
            for (auto* fr : f.ID3v2Tag()->frameList("APIC")) {
                auto* ap = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(fr);
                if (!ap) continue;
                if (!best) best = ap;
                if (ap->type() == TagLib::ID3v2::AttachedPictureFrame::FrontCover) { best = ap; break; }
            }
            if (!best) return std::nullopt;
            return blobFrom(best->picture(), best->mimeType().to8Bit(true));
        }
        if (ext == ".m4a" || ext == ".m4b" || ext == ".mp4") {
            TagLib::MP4::File f(TL_PATH(path), false);
            if (!f.isValid() || !f.tag()) return std::nullopt;
            TagLib::MP4::CoverArtList covers = f.tag()->item("covr").toCoverArtList();
            if (covers.isEmpty()) return std::nullopt;
            const TagLib::MP4::CoverArt& c = covers.front();
            std::string mime = (c.format() == TagLib::MP4::CoverArt::PNG) ? "image/png" : "image/jpeg";
            return blobFrom(c.data(), mime);
        }
        if (ext == ".opus" || ext == ".ogg") {
            TagLib::Ogg::Opus::File f(TL_PATH(path), false);
            if (!f.isValid() || !f.tag()) return std::nullopt;
            return fromPictureList(f.tag()->pictureList());
        }
        if (ext == ".wv") {
            TagLib::WavPack::File f(TL_PATH(path), false);
            if (!f.isValid() || !f.APETag()) return std::nullopt;
            // APE::Tag::itemListMap() upper-cases every key for lookup (the item
            // is still stored on disk as "Cover Art (Front)", matching the rip).
            const auto& m = f.APETag()->itemListMap();
            auto it = m.find("COVER ART (FRONT)");
            if (it == m.end()) return std::nullopt;
            TagLib::ByteVector bin = it->second.binaryData();
            int nul = bin.find('\0');                 // strip the "<filename>\0" prefix
            if (nul < 0) return std::nullopt;
            return blobFrom(bin.mid((unsigned)(nul + 1)), "");   // MIME from magic
        }
    } catch (...) {}
    return std::nullopt;   // WAV + unknown: no embedded art
}

bool embedArt(const std::string& path, RipFormat fmt, const ArtBlob& blob) {
    if (blob.bytes.empty()) return false;
    const std::string mime = blob.mime.empty() ? magicMime(blob.bytes) : blob.mime;
    TagLib::ByteVector bv((const char*)blob.bytes.data(), (unsigned)blob.bytes.size());
    try {
        switch (fmt) {
            case RipFormat::Mp3: {
                TagLib::MPEG::File f(TL_PATH(path), false);
                if (!f.isValid()) return false;
                auto* tag = f.ID3v2Tag(true); if (!tag) return false;
                auto* ap = new TagLib::ID3v2::AttachedPictureFrame();
                ap->setMimeType(mime);
                ap->setType(TagLib::ID3v2::AttachedPictureFrame::FrontCover);
                ap->setDescription("Cover");
                ap->setPicture(bv);
                tag->addFrame(ap);
                return f.save(TagLib::MPEG::File::ID3v2, TagLib::File::StripNone, TagLib::ID3v2::v4);
            }
            case RipFormat::Flac: {
                TagLib::FLAC::File f(TL_PATH(path), false);
                if (!f.isValid()) return false;
                auto* pic = new TagLib::FLAC::Picture();
                pic->setMimeType(mime);
                pic->setType(TagLib::FLAC::Picture::FrontCover);
                pic->setDescription("Cover");
                pic->setData(bv);
                f.addPicture(pic);
                return f.save();
            }
            case RipFormat::Opus: {
                TagLib::Ogg::Opus::File f(TL_PATH(path), false);
                if (!f.isValid() || !f.tag()) return false;
                auto* pic = new TagLib::FLAC::Picture();
                pic->setMimeType(mime);
                pic->setType(TagLib::FLAC::Picture::FrontCover);
                pic->setDescription("Cover");
                pic->setData(bv);
                f.tag()->addPicture(pic);
                return f.save();
            }
            case RipFormat::WavPack: {
                TagLib::WavPack::File f(TL_PATH(path), false);
                if (!f.isValid()) return false;
                auto* tag = f.APETag(true); if (!tag) return false;
                const char* fname = (mime == "image/png") ? "cover.png" : "cover.jpg";
                TagLib::ByteVector payload(fname);
                payload.append('\0');
                payload.append(bv);
                tag->setData("Cover Art (Front)", payload);
                return f.save();
            }
            case RipFormat::Wav:
            default:
                return false;   // WAV: no embedded-art concept
        }
    } catch (...) { return false; }
}
