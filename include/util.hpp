#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

void appendNetworkOrder8(std::vector<uint8_t>& packet, uint8_t byte);
void appendNetworkOrder16(std::vector<uint8_t>& packet, uint16_t value);
void appendNetworkOrder24(std::vector<uint8_t>& packet, uint32_t value);
void appendNetworkOrder32(std::vector<uint8_t>& packet, uint32_t value);
void appendNetworkOrder64(std::vector<uint8_t>& packet, uint64_t value);

std::vector<uint8_t> buildWritePayload(size_t len, uint8_t seed);