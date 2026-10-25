#pragma once

#include <cstdint>
#include <memory>
#include <string>

struct ID3D11Device;
struct IDXGISwapChain1;

// direct2d drawing shared by the console swapchain and the pc preview
namespace stardew_uwp
{
struct Bitmap;

struct Rect
{
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

inline Rect R(float left, float top, float right, float bottom)
{
    return Rect{left, top, right, bottom};
}

enum class Font
{
    Display,
    Title,
    Headline,
    Lead,
    Small,
    CardTitle,
    RowLarge,
    Row,
    RowStrong,
    Button,
    SmallMid,
    Overline,
    Value,
    Center,
    Key,
    Icon,
    IconLarge,
    Count,
};

// Bandit Launcher's default theme
struct Palette
{
    uint32_t bg = 0x0D0E10;
    uint32_t surface = 0x1B1D21;
    uint32_t raised = 0x2C2F35;
    uint32_t text = 0xF4F2ED;
    uint32_t muted = 0xA5A29C;
    uint32_t accent = 0xE3A96B;
    uint32_t danger = 0xEC7462;
    uint32_t onLight = 0x121315;
};

class Ui
{
public:
    Ui();
    ~Ui();
    Ui(const Ui &) = delete;
    Ui &operator=(const Ui &) = delete;

    bool AttachSwapchain(ID3D11Device *device, IDXGISwapChain1 *swapchain, std::string &log);
    bool CreateOffscreen(int width, int height, std::string &log);
    // everything holding the swapchain has to go before FNA3D asks for its own
    void Shutdown();

    bool Begin();
    bool End();
    bool SavePng(const std::wstring &path);

    float Width() const;
    float Height() const;
    const Palette &Colours() const;

    void Clear(uint32_t rgb);
    void Fill(Rect r, uint32_t rgb, float alpha = 1.0f);
    void FillRound(Rect r, float radius, uint32_t rgb, float alpha = 1.0f);
    void StrokeRound(Rect r, float radius, uint32_t rgb, float alpha, float width);
    void Circle(float cx, float cy, float radius, uint32_t rgb, float alpha = 1.0f);
    void Gradient(Rect r, uint32_t rgb, float alphaStart, float alphaEnd, bool vertical, float midPos = 0.5f, float midAlpha = -1.0f);

    float Text(const std::string &text, Font font, Rect r, uint32_t rgb, float alpha = 1.0f);
    float TextHeight(const std::string &text, Font font, float width);
    float TextWidth(const std::string &text, Font font);
    void Icon(const wchar_t *glyph, Rect r, uint32_t rgb, bool large = false);

    // the blur is done once on load and cached by path
    bool Picture(const std::wstring &path, Rect r, float opacity, float blur);
    bool Logo(const std::wstring &path, Rect r);
    void Thumbnail(int64_t key, const Bitmap &bitmap, Rect r);
    void ForgetThumbnails();
    void Qr(const std::string &text, Rect r);

private:
    struct State;
    std::unique_ptr<State> state_;
};
}
