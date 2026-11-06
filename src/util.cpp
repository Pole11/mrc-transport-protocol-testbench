#include "util.hpp"

void appendNetworkOrder8(std::vector<uint8_t>& packet, uint8_t byte)
{
    packet.push_back(byte);
}

void appendNetworkOrder16(std::vector<uint8_t>& packet, uint16_t value)
{
    appendNetworkOrder8(packet, static_cast<uint8_t>(value >> 8));
    appendNetworkOrder8(packet, static_cast<uint8_t>(value));
}

void appendNetworkOrder24(std::vector<uint8_t>& packet, uint32_t value)
{
    appendNetworkOrder8(packet, static_cast<uint8_t>(value >> 16));
    appendNetworkOrder16(packet, static_cast<uint16_t>(value));
}

void appendNetworkOrder32(std::vector<uint8_t>& packet, uint32_t value)
{
    appendNetworkOrder16(packet, static_cast<uint16_t>(value >> 16));
    appendNetworkOrder16(packet, static_cast<uint16_t>(value));
}

void appendNetworkOrder64(std::vector<uint8_t>& packet, uint64_t value)
{
    appendNetworkOrder32(packet, static_cast<uint32_t>(value >> 32));
    appendNetworkOrder32(packet, static_cast<uint32_t>(value));
}

std::vector<uint8_t> buildWritePayload(size_t len, uint8_t seed)
{
    std::vector<uint8_t> v(len);
    for (size_t i = 0; i < len; i++) v[i] = static_cast<uint8_t>(seed + i);
    return v;
}