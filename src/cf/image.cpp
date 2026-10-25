#include "image.h"

#include "net/http.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "third_party/stb/stb_image.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

namespace stardew_uwp
{
namespace
{
// logos come back at 256 square, and a card never draws one bigger than this
constexpr int kThumbSize = 128;

struct Fetcher
{
    std::mutex lock;
    std::condition_variable wake;
    std::thread worker;
    std::deque<std::pair<int64_t, std::string>> queue;
    std::vector<std::pair<int64_t, Bitmap>> ready;
    std::set<int64_t> seen;
    bool stopping = false;
    bool started = false;
};

Fetcher g_fetcher;

// box average, picking every other pixel of a 256 logo looks chewed on a tv
Bitmap Shrink(const unsigned char *rgba, int width, int height, int target)
{
    Bitmap out;
    if (width <= 0 || height <= 0)
    {
        return out;
    }

    out.width = target;
    out.height = target;
    out.bgra.assign(static_cast<std::size_t>(target) * static_cast<std::size_t>(target), 0u);

    for (int y = 0; y < target; y++)
    {
        const int sourceTop = y * height / target;
        const int sourceBottom = ((y + 1) * height / target) > sourceTop ? ((y + 1) * height / target) : sourceTop + 1;
        for (int x = 0; x < target; x++)
        {
            const int sourceLeft = x * width / target;
            const int sourceRight = ((x + 1) * width / target) > sourceLeft ? ((x + 1) * width / target) : sourceLeft + 1;

            unsigned int red = 0;
            unsigned int green = 0;
            unsigned int blue = 0;
            unsigned int count = 0;
            for (int sy = sourceTop; sy < sourceBottom && sy < height; sy++)
            {
                for (int sx = sourceLeft; sx < sourceRight && sx < width; sx++)
                {
                    const unsigned char *pixel = rgba + (static_cast<std::size_t>(sy) * width + sx) * 4;
                    // logos are often transparent around the edge, and the cards are dark
                    const unsigned int alpha = pixel[3];
                    red += pixel[0] * alpha / 255;
                    green += pixel[1] * alpha / 255;
                    blue += pixel[2] * alpha / 255;
                    count++;
                }
            }
            if (count == 0)
            {
                continue;
            }
            out.bgra[static_cast<std::size_t>(y) * target + x] =
                0xFF000000u | ((red / count) << 16) | ((green / count) << 8) | (blue / count);
        }
    }
    return out;
}

void Work()
{
    for (;;)
    {
        std::pair<int64_t, std::string> job;
        {
            std::unique_lock<std::mutex> guard(g_fetcher.lock);
            g_fetcher.wake.wait(guard, [] { return g_fetcher.stopping || !g_fetcher.queue.empty(); });
            if (g_fetcher.stopping)
            {
                return;
            }
            job = g_fetcher.queue.front();
            g_fetcher.queue.pop_front();
        }

        std::string body;
        // avatars are on the media cdn, which is public, so no key goes anywhere near this
        // a logo that fails to fetch or decode keeps the placeholder tile
        if (!HttpGetString(job.second, {}, body, 4 * 1024 * 1024).ok || body.empty())
        {
            continue;
        }

        int width = 0;
        int height = 0;
        int channels = 0;
        unsigned char *pixels = stbi_load_from_memory(
            reinterpret_cast<const unsigned char *>(body.data()),
            static_cast<int>(body.size()),
            &width,
            &height,
            &channels,
            4);
        if (pixels == nullptr)
        {
            continue;
        }

        Bitmap thumb = Shrink(pixels, width, height, kThumbSize);
        stbi_image_free(pixels);

        std::lock_guard<std::mutex> guard(g_fetcher.lock);
        g_fetcher.ready.emplace_back(job.first, std::move(thumb));
    }
}
}

void ThumbnailRequest(int64_t modId, const std::string &url)
{
    if (url.empty())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> guard(g_fetcher.lock);
        if (!g_fetcher.seen.insert(modId).second)
        {
            return;
        }
        g_fetcher.queue.emplace_back(modId, url);
        if (!g_fetcher.started)
        {
            g_fetcher.started = true;
            g_fetcher.worker = std::thread(Work);
        }
    }
    g_fetcher.wake.notify_all();
}

void ThumbnailTake(std::vector<std::pair<int64_t, Bitmap>> &out)
{
    std::lock_guard<std::mutex> guard(g_fetcher.lock);
    out.swap(g_fetcher.ready);
    g_fetcher.ready.clear();
}

void ThumbnailForget()
{
    std::lock_guard<std::mutex> guard(g_fetcher.lock);
    g_fetcher.queue.clear();
    g_fetcher.ready.clear();
    g_fetcher.seen.clear();
}

void ThumbnailStop()
{
    if (!g_fetcher.worker.joinable())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> guard(g_fetcher.lock);
        g_fetcher.stopping = true;
    }
    g_fetcher.wake.notify_all();
    g_fetcher.worker.join();
}
}
