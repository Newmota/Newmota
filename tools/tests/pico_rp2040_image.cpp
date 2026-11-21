#include "rp2040_image.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

constexpr std::uint32_t kRuntimeBase = 0x100C0000U;
constexpr std::uint32_t kSramBegin = 0x20000000U;
constexpr std::uint32_t kSramEnd = 0x20042000U;
constexpr std::size_t kImageSize = 512U;
constexpr std::size_t kBoot2Size = 0x100U;
constexpr std::size_t kBoot2PayloadSize = kBoot2Size - 4U;

using Image = std::array<std::uint8_t, kImageSize>;

// Captured from the first 256 bytes of the Pico W Safeboot image built with
// Pico SDK 2.3.1. This independent vector checks the helper against the SDK's
// pad_checksum boot2 trailer rather than against a synthetic image generated
// by that same helper.
constexpr std::array<std::uint8_t, kBoot2Size> kCapturedPicoWBoot2 = {
    0x00U,
    0xB5U,
    0x32U,
    0x4BU,
    0x21U,
    0x20U,
    0x58U,
    0x60U,
    0x98U,
    0x68U,
    0x02U,
    0x21U,
    0x88U,
    0x43U,
    0x98U,
    0x60U,
    0xD8U,
    0x60U,
    0x18U,
    0x61U,
    0x58U,
    0x61U,
    0x2EU,
    0x4BU,
    0x00U,
    0x21U,
    0x99U,
    0x60U,
    0x02U,
    0x21U,
    0x59U,
    0x61U,
    0x01U,
    0x21U,
    0xF0U,
    0x22U,
    0x99U,
    0x50U,
    0x2BU,
    0x49U,
    0x19U,
    0x60U,
    0x01U,
    0x21U,
    0x99U,
    0x60U,
    0x35U,
    0x20U,
    0x00U,
    0xF0U,
    0x44U,
    0xF8U,
    0x02U,
    0x22U,
    0x90U,
    0x42U,
    0x14U,
    0xD0U,
    0x06U,
    0x21U,
    0x19U,
    0x66U,
    0x00U,
    0xF0U,
    0x34U,
    0xF8U,
    0x19U,
    0x6EU,
    0x01U,
    0x21U,
    0x19U,
    0x66U,
    0x00U,
    0x20U,
    0x18U,
    0x66U,
    0x1AU,
    0x66U,
    0x00U,
    0xF0U,
    0x2CU,
    0xF8U,
    0x19U,
    0x6EU,
    0x19U,
    0x6EU,
    0x19U,
    0x6EU,
    0x05U,
    0x20U,
    0x00U,
    0xF0U,
    0x2FU,
    0xF8U,
    0x01U,
    0x21U,
    0x08U,
    0x42U,
    0xF9U,
    0xD1U,
    0x00U,
    0x21U,
    0x99U,
    0x60U,
    0x1BU,
    0x49U,
    0x19U,
    0x60U,
    0x00U,
    0x21U,
    0x59U,
    0x60U,
    0x1AU,
    0x49U,
    0x1BU,
    0x48U,
    0x01U,
    0x60U,
    0x01U,
    0x21U,
    0x99U,
    0x60U,
    0xEBU,
    0x21U,
    0x19U,
    0x66U,
    0xA0U,
    0x21U,
    0x19U,
    0x66U,
    0x00U,
    0xF0U,
    0x12U,
    0xF8U,
    0x00U,
    0x21U,
    0x99U,
    0x60U,
    0x16U,
    0x49U,
    0x14U,
    0x48U,
    0x01U,
    0x60U,
    0x01U,
    0x21U,
    0x99U,
    0x60U,
    0x01U,
    0xBCU,
    0x00U,
    0x28U,
    0x00U,
    0xD0U,
    0x00U,
    0x47U,
    0x12U,
    0x48U,
    0x13U,
    0x49U,
    0x08U,
    0x60U,
    0x03U,
    0xC8U,
    0x80U,
    0xF3U,
    0x08U,
    0x88U,
    0x08U,
    0x47U,
    0x03U,
    0xB5U,
    0x99U,
    0x6AU,
    0x04U,
    0x20U,
    0x01U,
    0x42U,
    0xFBU,
    0xD0U,
    0x01U,
    0x20U,
    0x01U,
    0x42U,
    0xF8U,
    0xD1U,
    0x03U,
    0xBDU,
    0x02U,
    0xB5U,
    0x18U,
    0x66U,
    0x18U,
    0x66U,
    0xFFU,
    0xF7U,
    0xF2U,
    0xFFU,
    0x18U,
    0x6EU,
    0x18U,
    0x6EU,
    0x02U,
    0xBDU,
    0x00U,
    0x00U,
    0x02U,
    0x40U,
    0x00U,
    0x00U,
    0x00U,
    0x18U,
    0x00U,
    0x00U,
    0x07U,
    0x00U,
    0x00U,
    0x03U,
    0x5FU,
    0x00U,
    0x21U,
    0x22U,
    0x00U,
    0x00U,
    0xF4U,
    0x00U,
    0x00U,
    0x18U,
    0x22U,
    0x20U,
    0x00U,
    0xA0U,
    0x00U,
    0x01U,
    0x00U,
    0x10U,
    0x08U,
    0xEDU,
    0x00U,
    0xE0U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    // Two pad bytes precede the little-endian checksum trailer at 0xfc.
    0x00U,
    0x00U,
    0x74U,
    0xB2U,
    0x4EU,
    0x7AU,
};

constexpr std::uint32_t read_u32(
    const std::array<std::uint8_t, kBoot2Size>& data, const std::size_t offset) {
    return static_cast<std::uint32_t>(data[offset]) |
           (static_cast<std::uint32_t>(data[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(data[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(data[offset + 3U]) << 24U);
}

void write_u32(Image& image, const std::size_t offset, const std::uint32_t value) {
    image[offset] = static_cast<std::uint8_t>(value);
    image[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
    image[offset + 2U] = static_cast<std::uint8_t>(value >> 16U);
    image[offset + 3U] = static_cast<std::uint8_t>(value >> 24U);
}

Image valid_image() {
    Image image{};
    for (std::size_t index = 0U; index < kBoot2PayloadSize; ++index) {
        image[index] = static_cast<std::uint8_t>((index * 37U + 11U) & 0xFFU);
    }
    const auto checksum =
        newmota::platform::pico_detail::rp2040_image_detail::boot2_checksum(image.data());
    write_u32(image, kBoot2PayloadSize, checksum);
    write_u32(image, kBoot2Size, kSramEnd);
    write_u32(image, kBoot2Size + 4U, kRuntimeBase + 0x111U);
    return image;
}

bool read_image(void* const context, const std::uint32_t offset, std::uint8_t* const output,
    const std::size_t size) {
    const auto& image = *static_cast<Image*>(context);
    if (offset > image.size() || size > image.size() - offset)
        return false;
    std::memcpy(output, image.data() + offset, size);
    return true;
}

bool valid(Image& image) {
    return newmota::platform::pico_detail::valid_rp2040_application(
        static_cast<std::uint32_t>(image.size()), kRuntimeBase, kSramBegin, kSramEnd, read_image,
        &image);
}

} // namespace

int main() {
    constexpr std::uint32_t kCapturedBoot2Checksum = 0x7A4EB274U;
    assert(newmota::platform::pico_detail::rp2040_image_detail::boot2_checksum(
               kCapturedPicoWBoot2.data()) == kCapturedBoot2Checksum);
    assert(read_u32(kCapturedPicoWBoot2, kBoot2PayloadSize) == kCapturedBoot2Checksum);

    auto image = valid_image();
    assert(newmota::platform::pico_detail::rp2040_image_detail::boot2_checksum(image.data()) ==
           0x476BE90AU);
    assert(valid(image));

    auto bad_checksum = valid_image();
    bad_checksum[100U] ^= 1U;
    assert(!valid(bad_checksum));

    auto bad_stack = valid_image();
    write_u32(bad_stack, kBoot2Size, 0x21000000U);
    assert(!valid(bad_stack));

    auto bad_reset = valid_image();
    write_u32(bad_reset, kBoot2Size + 4U, kRuntimeBase + 0xF1U);
    assert(!valid(bad_reset));

    auto non_thumb_reset = valid_image();
    write_u32(non_thumb_reset, kBoot2Size + 4U, kRuntimeBase + 0x110U);
    assert(!valid(non_thumb_reset));

    auto out_of_image_reset = valid_image();
    write_u32(out_of_image_reset, kBoot2Size + 4U, kRuntimeBase + 0x211U);
    assert(!valid(out_of_image_reset));

    assert(!newmota::platform::pico_detail::valid_rp2040_application(
        static_cast<std::uint32_t>(kBoot2Size + 4U), kRuntimeBase, kSramBegin, kSramEnd, read_image,
        &image));
    return 0;
}
