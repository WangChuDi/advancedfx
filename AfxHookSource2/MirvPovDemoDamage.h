#pragma once

// A bounded, dependency-free reader for Source 2 demo damage events. It never
// touches game entities or Panorama; callers may build an index on a worker.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace MirvPovDemoDamage {
constexpr size_t MaxPayload = 64 * 1024 * 1024;

struct View {
    const uint8_t* data;
    size_t size;
};

inline uint64_t Varint(View b, size_t& p) {
    uint64_t value = 0;
    for(unsigned shift = 0; shift < 64; shift += 7) {
        if(p == b.size) throw std::runtime_error("truncated varint");
        const unsigned byte = b.data[p++];
        if(shift == 63 && byte > 1) throw std::runtime_error("varint overflow");
        value |= uint64_t(byte & 127) << shift;
        if(!(byte & 128)) return value;
    }
    throw std::runtime_error("varint overflow");
}

struct Field {
    unsigned number = 0, wire = 0;
    uint64_t integer = 0;
    View bytes = {};
};

class Proto {
    View m_Data;
    size_t m_Pos = 0;
public:
    explicit Proto(View data) : m_Data(data) {}
    bool Next(Field& f) {
        if(m_Pos == m_Data.size) return false;
        const uint64_t key = Varint(m_Data, m_Pos);
        f = {}; f.number = unsigned(key >> 3); f.wire = unsigned(key & 7);
        if(!f.number) throw std::runtime_error("invalid protobuf field");
        if(f.wire == 0) { f.integer = Varint(m_Data, m_Pos); return true; }
        size_t length = 0;
        if(f.wire == 1) length = 8;
        else if(f.wire == 5) length = 4;
        else if(f.wire == 2) {
            const uint64_t n = Varint(m_Data, m_Pos);
            if(n > MaxPayload) throw std::runtime_error("protobuf too large");
            length = size_t(n);
        } else throw std::runtime_error("unsupported protobuf wire");
        if(length > m_Data.size - m_Pos) throw std::runtime_error("truncated protobuf");
        f.bytes = {m_Data.data + m_Pos, length}; m_Pos += length;
        return true;
    }
};

inline View Bytes(View b, unsigned number) {
    Proto p(b); Field f;
    while(p.Next(f)) if(f.number == number && f.wire == 2) return f.bytes;
    return {};
}

inline std::string String(View b) {
    return b.size ? std::string(reinterpret_cast<const char*>(b.data), b.size) : std::string();
}

inline std::vector<uint8_t> Snappy(View b) {
    size_t p = 0; const uint64_t n = Varint(b, p);
    if(n > MaxPayload) throw std::runtime_error("snappy too large");
    std::vector<uint8_t> out; out.reserve(size_t(n));
    while(p < b.size) {
        unsigned tag = b.data[p++], kind = tag & 3;
        size_t length, offset = 0;
        if(!kind) {
            length = tag >> 2;
            if(length >= 60) {
                const size_t width = length - 59; length = 0;
                if(width > b.size - p) throw std::runtime_error("truncated snappy literal");
                for(size_t i = 0; i < width; ++i) length |= size_t(b.data[p++]) << (8 * i);
            }
            ++length;
            if(length > b.size - p || length > n - out.size()) throw std::runtime_error("invalid snappy literal");
            out.insert(out.end(), b.data + p, b.data + p + length); p += length;
        } else {
            const size_t width = kind == 1 ? 1 : kind == 2 ? 2 : 4;
            if(width > b.size - p) throw std::runtime_error("truncated snappy copy");
            if(kind == 1) { length = 4 + ((tag >> 2) & 7); offset = (tag & 224) << 3; }
            else length = 1 + (tag >> 2);
            for(size_t i = 0; i < width; ++i) offset |= size_t(b.data[p++]) << (8 * i);
            if(!offset || offset > out.size() || length > n - out.size()) throw std::runtime_error("invalid snappy copy");
            for(size_t i = 0; i < length; ++i) out.push_back(out[out.size() - offset]);
        }
    }
    if(out.size() != n) throw std::runtime_error("snappy length mismatch");
    return out;
}

class Bits {
    View m_Data;
    size_t m_Pos = 0;
public:
    explicit Bits(View data) : m_Data(data) {}
    size_t Remaining() const { return m_Data.size * 8 - m_Pos; }
    uint32_t Read(unsigned count) {
        if(count > 32 || count > Remaining()) throw std::runtime_error("truncated packet");
        uint32_t value = 0;
        for(unsigned i = 0; i < count; ++i, ++m_Pos)
            value |= uint32_t((m_Data.data[m_Pos / 8] >> (m_Pos % 8)) & 1) << i;
        return value;
    }
    uint32_t UBitVar() {
        const uint32_t value = Read(6);
        const unsigned widths[] = {0, 4, 8, 28};
        return (value >> 4) ? (value & 15) | (Read(widths[value >> 4]) << 4) : value;
    }
    uint32_t Var() {
        uint32_t value = 0;
        for(unsigned shift = 0; shift < 35; shift += 7) {
            const uint32_t byte = Read(8);
            if(shift == 28 && byte > 15) throw std::runtime_error("packet varint overflow");
            value |= (byte & 127) << shift;
            if(!(byte & 128)) return value;
        }
        throw std::runtime_error("packet varint overflow");
    }
    void Take(std::vector<uint8_t>& out, size_t n) {
        if(n > MaxPayload || n > Remaining() / 8) throw std::runtime_error("packet too large");
        out.resize(n);
        for(size_t i = 0; i < n; ++i) out[i] = uint8_t(Read(8));
    }
    void Skip(size_t n) {
        if(n > Remaining() / 8) throw std::runtime_error("truncated message");
        m_Pos += n * 8;
    }
};

struct Damage {
    int health = 0, hits = 0;
    uint8_t kill = 0;
};
struct Hit {
    int tick, attacker, victim, health;
    uint8_t kill;
};
struct Round {
    int start = 0, end = -1;
    std::map<std::pair<int, int>, Damage> pairs;
    std::vector<Hit> events;
    std::map<int, int> health;
    Damage At(int attacker, int victim, int tick) const {
        Damage result;
        for(const auto& event : events) {
            if(event.tick > tick) break;
            if(event.attacker != attacker || event.victim != victim) continue;
            if(event.health > 0) { result.health += event.health; ++result.hits; }
            if(event.kill) result.kill = event.kill;
        }
        return result;
    }
};
struct Descriptor {
    std::string name;
    std::vector<std::string> keys;
};
struct Value {
    int type = 0, integer = -1;
    std::string text;
};

class Index {
    std::map<int, Descriptor> m_Descriptors;
    std::vector<Round> m_Rounds;
    bool m_HasPrestart = false;
    void Descriptors(View b) {
        Proto list(b); Field f;
        while(list.Next(f)) if(f.number == 1 && f.wire == 2) {
            int id = -1; Descriptor descriptor; Proto item(f.bytes); Field d;
            while(item.Next(d)) {
                if(d.number == 1 && d.wire == 0) id = int(d.integer);
                else if(d.number == 2 && d.wire == 2) descriptor.name = String(d.bytes);
                else if(d.number == 3 && d.wire == 2) descriptor.keys.push_back(String(Bytes(d.bytes, 2)));
            }
            if(id >= 0 && descriptor.keys.size() <= 256) m_Descriptors[id] = std::move(descriptor);
        }
    }
    void Event(View b, int tick) {
        int id = -1; std::vector<Value> values; Proto p(b); Field f;
        while(p.Next(f)) {
            if(f.number == 2 && f.wire == 0) id = int(f.integer);
            else if(f.number == 3 && f.wire == 2) {
                Value value; Proto key(f.bytes); Field k;
                while(key.Next(k)) {
                    if(k.number == 1 && k.wire == 0) value.type = int(k.integer);
                    else if(k.number == 2 && k.wire == 2) value.text = String(k.bytes);
                    else if(k.number >= 4 && k.number <= 7 && k.wire == 0) value.integer = int(k.integer);
                }
                if(values.size() >= 256) throw std::runtime_error("too many event keys");
                values.push_back(std::move(value));
            }
        }
        const auto found = m_Descriptors.find(id);
        if(found == m_Descriptors.end()) throw std::runtime_error("game event descriptors missing");
        const auto& descriptor = found->second;
        auto get = [&](const char* name) -> Value {
            for(size_t i = 0; i < descriptor.keys.size() && i < values.size(); ++i)
                if(descriptor.keys[i] == name) return values[i];
            return {};
        };
        // GOTV does not necessarily record round_start/end. round_prestart is
        // the authoritative boundary in that stream; runtime GameRules decides
        // whether this round has actually ended before the HUD calls At().
        if(descriptor.name == "round_prestart" || (descriptor.name == "round_start" && !m_HasPrestart)) {
            if(descriptor.name == "round_prestart") m_HasPrestart = true;
            if(m_Rounds.size() >= 1024) throw std::runtime_error("too many demo rounds");
            if(!m_Rounds.empty() && m_Rounds.back().start > tick) throw std::runtime_error("unordered demo rounds");
            if(m_Rounds.empty() || m_Rounds.back().start != tick) m_Rounds.push_back({tick, -1, {}, {}, {}});
            return;
        }
        if(m_Rounds.empty()) return;
        auto& round = m_Rounds.back();
        if(descriptor.name == "round_end") { round.end = tick; return; }
        if(descriptor.name == "player_spawn") {
            const auto player = get("userid");
            if(player.type == 9 && player.integer >= 0 && player.integer < 64) round.health[player.integer] = 100;
            return;
        }
        if(descriptor.name != "player_hurt" && descriptor.name != "player_death") return;
        if(!round.events.empty() && round.events.back().tick > tick) throw std::runtime_error("unordered damage events");
        // Source2 type 9 is a zero-based CPlayerSlot, not Source1's userid.
        const Value attacker = get("attacker"), victim = get("userid");
        int removed = 0;
        if(descriptor.name == "player_hurt" && victim.type == 9 && victim.integer >= 0 && victim.integer < 64) {
            const int reported = get("dmg_health").integer, remaining = get("health").integer;
            const auto previous = round.health.find(victim.integer);
            const int before = previous == round.health.end() ? 100 : previous->second;
            // The native publisher reads m_flActualHealthRemoved (+0x68), not
            // m_flDamage (+0x64). Do not display lethal overkill as lost HP.
            if(reported > 0 && reported <= 10000 && remaining >= 0 && remaining <= 10000)
                removed = remaining == 0 && before < reported ? before : reported;
            if(remaining >= 0 && remaining <= 10000) round.health[victim.integer] = remaining;
        }
        if(attacker.type != 9 || victim.type != 9 || attacker.integer < 0 || attacker.integer >= 64
            || victim.integer < 0 || victim.integer >= 64 || attacker.integer == victim.integer) return;
        auto& damage = round.pairs[{attacker.integer, victim.integer}];
        if(round.events.size() >= 16384) throw std::runtime_error("too many round damage events");
        if(descriptor.name == "player_hurt") {
            if(removed > 0 && damage.health <= 1000000 - removed) {
                damage.health += removed; ++damage.hits;
                round.events.push_back({tick, attacker.integer, victim.integer, removed, 0});
            }
        } else {
            const std::string weapon = get("weapon").text;
            damage.kill = 1;
            if(get("headshot").integer == 1) damage.kill = 2;
            else if(weapon == "hegrenade" || weapon == "c4") damage.kill = 3;
            else if(weapon == "inferno" || weapon == "molotov" || weapon == "incgrenade") damage.kill = 4;
            else if(weapon.find("knife") != std::string::npos || weapon == "bayonet") damage.kill = 5;
            else if(weapon == "taser") damage.kill = 6;
            round.events.push_back({tick, attacker.integer, victim.integer, 0, damage.kill});
        }
    }
public:
    const std::vector<Round>& Rounds() const { return m_Rounds; }
    const Round* At(int tick) const {
        for(size_t i = m_Rounds.size(); i > 0; --i) {
            const auto& round = m_Rounds[i - 1];
            if(round.start <= tick) return &round;
        }
        return nullptr;
    }
    void Read(std::istream& file, const std::atomic<bool>& cancel) {
        char header[16]; file.read(header, sizeof(header));
        if(!file || memcmp(header, "PBDEMS2\0", 8)) throw std::runtime_error("not a Source2 demo");
        auto var = [&]() -> uint32_t {
            uint32_t value = 0;
            for(unsigned shift = 0; shift < 35; shift += 7) {
                const int byte = file.get();
                if(byte < 0 || (shift == 28 && byte > 15)) throw std::runtime_error("invalid demo frame");
                value |= uint32_t(byte & 127) << shift;
                if(!(byte & 128)) return value;
            }
            throw std::runtime_error("invalid demo varint");
        };
        std::vector<uint8_t> raw, decoded, message;
        while(file.peek() != EOF) {
            if(cancel.load()) throw std::runtime_error("cancelled");
            const unsigned command = var(); const int tick = int(var()); const unsigned size = var();
            if(size > MaxPayload) throw std::runtime_error("demo frame too large");
            const unsigned kind = command & ~64u;
            if(kind == 0) break;
            if(kind != 7 && kind != 8 && kind != 13) {
                file.ignore(size);
                if(file.gcount() != size) throw std::runtime_error("truncated demo frame");
                continue;
            }
            raw.resize(size); file.read(reinterpret_cast<char*>(raw.data()), size);
            if(!file) throw std::runtime_error("truncated demo packet");
            View payload = {raw.data(), raw.size()};
            if(command & 64) { decoded = Snappy(payload); payload = {decoded.data(), decoded.size()}; }
            if(kind == 13) payload = Bytes(payload, 2);
            Bits bits(Bytes(payload, 3));
            while(bits.Remaining() >= 14) {
                const unsigned type = bits.UBitVar(), length = bits.Var();
                // Full packets are snapshots; replaying their game events would
                // count historical damage twice. Descriptors remain usable.
                if(type == 205 || (type == 207 && kind != 13)) {
                    bits.Take(message, length); const View body = {message.data(), message.size()};
                    if(type == 205) Descriptors(body); else if(tick >= 0) Event(body, tick);
                } else bits.Skip(length);
            }
        }
        if(m_Descriptors.empty()) throw std::runtime_error("game event descriptors missing");
        if(m_Rounds.empty()) throw std::runtime_error("round boundaries missing");
    }
};
} // namespace MirvPovDemoDamage
