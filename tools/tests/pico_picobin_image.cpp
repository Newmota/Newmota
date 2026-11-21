#include "picobin_image.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr std::uint32_t kRuntimeBase = 0x100D0000U;
constexpr std::uint32_t kSramBegin = 0x20000000U;
constexpr std::uint32_t kSramEnd = 0x20082000U;
constexpr std::size_t kFirstBlock = 32U;
constexpr std::size_t kSecondBlock = 64U;

void write_u32(
    std::vector<std::uint8_t>& image, const std::size_t offset, const std::uint32_t value) {
    image[offset] = static_cast<std::uint8_t>(value);
    image[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
    image[offset + 2U] = static_cast<std::uint8_t>(value >> 16U);
    image[offset + 3U] = static_cast<std::uint8_t>(value >> 24U);
}

void write_u16(
    std::vector<std::uint8_t>& image, const std::size_t offset, const std::uint16_t value) {
    image[offset] = static_cast<std::uint8_t>(value);
    image[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
}

void write_image_block(std::vector<std::uint8_t>& image, const std::size_t offset,
    const std::uint32_t next_offset, const bool image_type, const bool partition_table = false) {
    write_u32(image, offset, 0xFFFFDED3U);
    image[offset + 4U] = partition_table ? 0x0AU : (image_type ? 0x42U : 0xFEU);
    image[offset + 5U] = 1U;
    image[offset + 6U] = image_type ? 0x21U : 0U;
    image[offset + 7U] = image_type ? 0x10U : 0U;
    image[offset + 8U] = 0xFFU;
    write_u16(image, offset + 9U, 1U);
    image[offset + 11U] = 0U;
    write_u32(image, offset + 12U, next_offset - static_cast<std::uint32_t>(offset));
    write_u32(image, offset + 16U, 0xAB123579U);
}

std::vector<std::uint8_t> single_block_image() {
    std::vector<std::uint8_t> image(128U, 0xFFU);
    write_u32(image, 0U, kSramEnd);
    write_u32(image, 4U, kRuntimeBase + 0x11U);
    write_image_block(image, kFirstBlock, static_cast<std::uint32_t>(kFirstBlock), true);
    return image;
}

std::vector<std::uint8_t> linked_image() {
    auto image = single_block_image();
    write_image_block(image, kFirstBlock, static_cast<std::uint32_t>(kSecondBlock), true);
    write_image_block(image, kSecondBlock, static_cast<std::uint32_t>(kFirstBlock), false);
    return image;
}

bool read_image(void* const context, const std::uint32_t offset, std::uint8_t* const output,
    const std::size_t size) {
    const auto& image = *static_cast<std::vector<std::uint8_t>*>(context);
    if (offset > image.size() || size > image.size() - offset)
        return false;
    std::memcpy(output, image.data() + offset, size);
    return true;
}

bool valid(std::vector<std::uint8_t> image) {
    return newmota::platform::pico_detail::valid_rp2350_application(
        static_cast<std::uint32_t>(image.size()), kRuntimeBase, kSramBegin, kSramEnd, read_image,
        &image);
}

} // namespace

int main() {
    assert(valid(single_block_image()));
    assert(valid(linked_image()));

    auto malformed_item = single_block_image();
    malformed_item[kFirstBlock + 5U] = 0U;
    assert(!valid(malformed_item));

    auto wrong_architecture = single_block_image();
    write_u16(wrong_architecture, kFirstBlock + 6U, 0x1101U);
    assert(!valid(wrong_architecture));

    auto embedded_table = linked_image();
    write_image_block(
        embedded_table, kSecondBlock, static_cast<std::uint32_t>(kFirstBlock), false, true);
    assert(!valid(embedded_table));

    auto broken_loop = linked_image();
    write_u32(broken_loop, kSecondBlock + 12U, 0U);
    assert(!valid(broken_loop));

    auto wrong_vector = single_block_image();
    write_u32(wrong_vector, 4U, 0x100C0011U);
    assert(!valid(wrong_vector));

    auto invalid_stack = single_block_image();
    write_u32(invalid_stack, 0U, 0x21000000U);
    assert(!valid(invalid_stack));
    return 0;
}
