// ID3v1 and APEv2 tags at the end of a stream, read the way the official library does (ID3v1
// first, then APEv2 in front of it, which overrides shared keys) but with every length checked.
#include "../../include/optimfrog_decoder.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace {

constexpr size_t MAX_TAGS = 64;
constexpr uint32_t APE_VERSION = 2000;

const char* const id3_genres[] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz",
    "Metal", "New Age", "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno",
    "Industrial", "Alternative", "Ska", "Death Metal", "Pranks", "Soundtrack", "Euro-Techno",
    "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical", "Instrumental",
    "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise", "Altern Rock", "Bass", "Soul",
    "Punk", "Space", "Meditative", "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic",
    "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream",
    "Southern Rock", "Comedy", "Cult", "Gangsta", "Top 40", "Christian Rap", "Pop/Funk", "Jungle",
    "Native American", "Cabaret", "New Wave", "Psychadelic", "Rave", "Showtunes", "Trailer",
    "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll",
    "Hard Rock", "Folk", "Folk/Rock", "National Folk", "Fast Fusion", "Swing", "Bebob", "Latin",
    "Revival", "Celtic", "Bluegrass", "Avantgarde", "Gothic Rock", "Progressive Rock",
    "Psychedelic Rock", "Symphonic Rock", "Slow Rock", "Big Band", "Chorus", "Easy Listening",
    "Acoustic", "Humour", "Speech", "Chanson", "Opera", "Chamber Music", "Sonata", "Symphony",
    "Booty Bass", "Primus", "Porn Groove", "Satire", "Slow Jam", "Club", "Tango", "Samba",
    "Folklore", "Ballad", "Power Ballad", "Rhythmic Soul", "Freestyle", "Duet", "Punk Rock",
    "Drum Solo", "A Capella", "Euro-House", "Dance Hall", "Goa", "Drum & Bass", "Club-House",
    "Hardcore", "Terror", "Indie", "BritPop", "Negerpunk", "Polsk Punk", "Beat",
    "Christian Gangsta Rap", "Heavy Metal", "Black Metal", "Crossover", "Contemporary Christian",
    "Christian Rock", "Merengue", "Salsa", "Thrash Metal", "Anime", "JPop", "Synthpop",
};

uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool same_key(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return std::tolower((unsigned char)x) == std::tolower((unsigned char)y);
    });
}

std::string trimmed(std::string s) {
    size_t first = s.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(' ') - first + 1);
}

void add_tag(OFR_TagList& tags, const std::string& key, const std::string& value) {
    if (key.empty() || value.empty()) return;
    for (auto& tag : tags) {
        if (same_key(tag.first, key)) {
            tag.second = value;
            return;
        }
    }
    if (tags.size() < MAX_TAGS) tags.emplace_back(key, value);
}

// fixed-size ID3v1 field: up to the first NUL
std::string field(const uint8_t* p, size_t n) {
    return trimmed(std::string((const char*)p, strnlen((const char*)p, n)));
}

bool read_id3v1(OFR_BitStream& bs, uint64_t length, OFR_TagList& tags) {
    uint8_t t[128];
    if (length < sizeof(t) || !bs.seek(length - sizeof(t)) || bs.read(t, sizeof(t)) != sizeof(t)) return false;
    if (std::memcmp(t, "TAG", 3) != 0) return false;

    add_tag(tags, "Title", field(t + 3, 30));
    add_tag(tags, "Artist", field(t + 33, 30));
    add_tag(tags, "Album", field(t + 63, 30));
    add_tag(tags, "Year", field(t + 93, 4));
    if (t[125] != 0) {
        add_tag(tags, "Comment", field(t + 97, 30));
    } else {
        add_tag(tags, "Comment", field(t + 97, 28));
        if (t[126] != 0) add_tag(tags, "Track", std::to_string(t[126]));
    }
    if (t[127] != 0xff) {
        constexpr size_t genres = sizeof(id3_genres) / sizeof(id3_genres[0]);
        add_tag(tags, "Genre", t[127] < genres ? id3_genres[t[127]] : "Invalid");
    }
    return true;
}

void read_apev2(OFR_BitStream& bs, uint64_t end, OFR_TagList& tags) {
    uint8_t footer[32];
    if (end < sizeof(footer) || !bs.seek(end - sizeof(footer)) || bs.read(footer, sizeof(footer)) != sizeof(footer)) return;
    // bit 29 marks a header, bit 30 a tag without a footer: neither can be what ends the file
    if (std::memcmp(footer, "APETAGEX", 8) != 0 || le32(footer + 8) != APE_VERSION || (footer[23] & 0x60) != 0) return;

    uint32_t size = le32(footer + 12), count = le32(footer + 16);
    if (size > end || !bs.seek(end - size)) return;
    std::vector<uint8_t> body;
    if (bs.read_append(body, size) != size) return;

    size_t p = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (body.size() - p < 9) return;
        uint32_t value_size = le32(&body[p]), flags = le32(&body[p + 4]);
        const uint8_t* key = &body[p + 8];
        const uint8_t* key_end = (const uint8_t*)std::memchr(key, 0, body.size() - p - 8);
        if (!key_end) return;
        size_t value_pos = (size_t)(key_end - body.data()) + 1;
        if (value_size > body.size() - value_pos) return;

        // item type 1 is binary data
        if ((flags & 6) != 2) {
            const char* value = (const char*)&body[value_pos];
            add_tag(tags, std::string((const char*)key), trimmed(std::string(value, strnlen(value, value_size))));
        }
        p = value_pos + value_size;
    }
}

} // namespace

OFR_TagList ofr_read_tags(OFR_BitStream& bs) {
    OFR_TagList tags;
    if (!bs.seekable()) return tags;

    uint64_t saved = bs.tell();
    int64_t length = bs.length();
    if (length > 0) {
        bool has_id3 = read_id3v1(bs, (uint64_t)length, tags);
        read_apev2(bs, (uint64_t)length - (has_id3 ? 128 : 0), tags);
    }
    bs.seek(saved);
    return tags;
}
