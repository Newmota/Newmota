#include "image_size.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr std::uint32_t kSlotSize = 16384U;
constexpr std::array<std::uint32_t, 5> kHeaderOffsets{0U, 132U, 8192U, 10224U, 13856U};
constexpr std::array<std::uint32_t, 5> kPayloadSizes{100U, 6000U, 2000U, 3600U, 0U};

void write_u32(std::uint8_t* bytes, const std::uint32_t value) {
    for (unsigned index = 0U; index < 4U; ++index) {
        bytes[index] = static_cast<std::uint8_t>(value >> (index * 8U));
    }
}

struct Fixture final {
    std::array<std::uint8_t, kSlotSize> image{};
    unsigned reads{};
    unsigned fail_read{5U};

    Fixture() {
        image.fill(0xFFU);
        for (std::size_t index = 0U; index < kHeaderOffsets.size(); ++index) {
            auto* header = image.data() + kHeaderOffsets[index];
            write_u32(header, 0x35393138U);
            write_u32(header + 4U, 0x31313738U);
            write_u32(header + 8U, kPayloadSizes[index]);
        }
    }
};

bool read_at(
    void* context, const std::uint32_t offset, std::uint8_t* bytes, const std::size_t size) {
    auto& fixture = *static_cast<Fixture*>(context);
    assert(fixture.reads < kHeaderOffsets.size());
    assert(offset == kHeaderOffsets[fixture.reads]);
    assert(size == 32U && offset <= fixture.image.size());
    assert(size <= fixture.image.size() - offset);
    if (fixture.reads++ == fixture.fail_read)
        return false;
    std::memcpy(bytes, fixture.image.data() + offset, size);
    return true;
}

bool overflowing_lp(
    void* context, const std::uint32_t offset, std::uint8_t* bytes, const std::size_t size) {
    auto& reads = *static_cast<unsigned*>(context);
    assert(size == 32U && reads < 2U);
    assert(offset == (reads == 0U ? 0U : UINT32_MAX - 32U));
    std::memset(bytes, 0xFF, size);
    write_u32(bytes, 0x35393138U);
    write_u32(bytes + 4U, 0x31313738U);
    write_u32(bytes + 8U, reads == 0U ? UINT32_MAX - 64U : 0U);
    ++reads;
    return true;
}

} // namespace

int main() {
    using newmota::platform::amebad_detail::combined_image_size;
    {
        Fixture fixture;
        std::uint32_t size = 0U;
        assert(combined_image_size(kSlotSize, read_at, &fixture, size));
        // LP is padded from 6164 to 8192 bytes; HP from 5696 to 8192.
        // Both core images and both padding ranges occupy the installed slot.
        assert(size == kSlotSize && fixture.reads == 5U);
    }
    {
        Fixture fixture;
        write_u32(fixture.image.data() + kHeaderOffsets[4] + 8U, 2500U);
        std::uint32_t size = 0U;
        assert(combined_image_size(20480U, read_at, &fixture, size));
        assert(size == 20480U && fixture.reads == 5U);
    }
    for (std::size_t index = 0U; index < kHeaderOffsets.size(); ++index) {
        Fixture fixture;
        fixture.image[kHeaderOffsets[index]] ^= 1U;
        std::uint32_t size = 777U;
        assert(!combined_image_size(kSlotSize, read_at, &fixture, size));
        assert(size == 777U);
    }
    for (unsigned index = 0U; index < 5U; ++index) {
        Fixture fixture;
        fixture.fail_read = index;
        std::uint32_t size = 777U;
        assert(!combined_image_size(kSlotSize, read_at, &fixture, size));
        assert(size == 777U);
    }
    for (const auto capacity : {0U, 31U, 8191U, 13887U, kSlotSize - 1U}) {
        Fixture fixture;
        std::uint32_t size = 777U;
        assert(!combined_image_size(capacity, read_at, &fixture, size));
        assert(size == 777U);
    }
    for (const auto offset : kHeaderOffsets) {
        Fixture fixture;
        write_u32(fixture.image.data() + offset + 8U, UINT32_MAX);
        std::uint32_t size = 777U;
        assert(!combined_image_size(kSlotSize, read_at, &fixture, size));
        assert(size == 777U);
    }
    {
        Fixture fixture;
        // A secure HP extension has more content than these five headers.
        write_u32(fixture.image.data() + kHeaderOffsets[2] + 16U, 0U);
        std::uint32_t size = 777U;
        assert(!combined_image_size(kSlotSize, read_at, &fixture, size));
        assert(size == 777U);
    }
    {
        unsigned reads = 0U;
        std::uint32_t size = 777U;
        assert(!combined_image_size(UINT32_MAX, overflowing_lp, &reads, size));
        assert(reads == 2U && size == 777U);
        assert(!combined_image_size(kSlotSize, nullptr, nullptr, size));
    }
    std::puts("AmebaD combined image size tests passed");
}
