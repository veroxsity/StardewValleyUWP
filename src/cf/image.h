#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace stardew_uwp
{
struct Bitmap
{
    int width = 0;
    int height = 0;
    std::vector<uint32_t> bgra;
};

// decoded off the draw thread and handed over once, so a frame never copies or locks
void ThumbnailRequest(int64_t modId, const std::string &url);
void ThumbnailTake(std::vector<std::pair<int64_t, Bitmap>> &out);
// drops queued and finished logos and lets a mod be fetched again once its old one is gone
void ThumbnailForget();
void ThumbnailStop();
}
