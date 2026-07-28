// Unit tests for ImageBuffer::valid(), which had no coverage. valid() accepts
// a buffer whose dimensions and stride are non-negative, whose pixel vector
// holds exactly width*height entries, and whose stride spans at least one row
// (width*4 bytes). Every branch below is exercised; the width*height ->
// size_t overflow guard is 32-bit-only (int*int cannot exceed uint64 on a
// 64-bit size_t) and is therefore not reachable to test here.
#include <amrexplorer/render2d/ImageBuffer.hpp>

#include <cstddef>
#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

// Assembles a buffer with an rgba vector of exactly `pixels` entries, so each
// case sets width/height/stride and the pixel count independently.
amrvis::ImageBuffer make(int width, int height, int strideBytes,
    std::size_t pixels)
{
    amrvis::ImageBuffer buffer;
    buffer.width = width;
    buffer.height = height;
    buffer.strideBytes = strideBytes;
    buffer.rgba.assign(pixels, 0u);
    return buffer;
}

} // namespace

int main()
{
    // A default-constructed buffer is 0x0 with an empty pixel vector, and that
    // is deliberately valid(): callers that must tell "no image" from "valid
    // empty image" check emptiness, not validity (see the showSlice
    // invalid-image note in mainwindow-needs-extraction.md).
    require(amrvis::ImageBuffer{}.valid(), "a default 0x0 buffer should be valid");

    // Well-formed: stride exactly width*4, and a padded (larger) stride.
    require(make(2, 3, 8, 6).valid(), "a tight 2x3 buffer should be valid");
    require(make(2, 3, 16, 6).valid(),
        "a 2x3 buffer with a padded stride should be valid");

    // Negative dimensions are rejected on each axis independently.
    require(!make(-1, 3, 8, 0).valid(), "negative width should be invalid");
    require(!make(2, -1, 8, 0).valid(), "negative height should be invalid");
    require(!make(2, 3, -1, 6).valid(), "negative stride should be invalid");

    // The pixel vector must hold exactly width*height entries.
    require(!make(2, 3, 8, 5).valid(),
        "an rgba vector shorter than width*height should be invalid");
    require(!make(2, 3, 8, 7).valid(),
        "an rgba vector longer than width*height should be invalid");

    // The stride must span at least one full row of pixels.
    require(!make(2, 3, 4, 6).valid(),
        "a stride below width*4 should be invalid");

    // Degenerate extents hold no pixels but the stride rule still applies to
    // the (possibly zero) width.
    require(make(0, 5, 0, 0).valid(), "a 0x5 empty buffer should be valid");
    require(make(5, 0, 20, 0).valid(),
        "a 5x0 empty buffer with a full-width stride should be valid");
    require(!make(5, 0, 0, 0).valid(),
        "a 5x0 buffer whose stride cannot hold one row should be invalid");

    return 0;
}
