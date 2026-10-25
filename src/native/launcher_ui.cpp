#include "launcher_ui.h"

#include "cf/image.h"
#include "text.h"

#include "third_party/qrcodegen/qrcodegen.h"

#include <windows.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <wrl.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace stardew_uwp
{
namespace
{
// a steam sign in link and a lan address both fit with room to spare
constexpr int kQrMaxVersion = 10;

struct FontSpec
{
    const wchar_t *family;
    DWRITE_FONT_WEIGHT weight;
    float size;
    DWRITE_TEXT_ALIGNMENT align;
    bool centreVertically;
    bool wrap;
};

// sizes and weights copied from Bandit Launcher
const FontSpec kFonts[] = {
    {L"Segoe UI", DWRITE_FONT_WEIGHT_LIGHT, 62.0f, DWRITE_TEXT_ALIGNMENT_LEADING, false, false},
    {L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 40.0f, DWRITE_TEXT_ALIGNMENT_LEADING, false, false},
    {L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 40.0f, DWRITE_TEXT_ALIGNMENT_LEADING, false, true},
    {L"Segoe UI", DWRITE_FONT_WEIGHT_REGULAR, 25.0f, DWRITE_TEXT_ALIGNMENT_LEADING, false, true},
    {L"Segoe UI", DWRITE_FONT_WEIGHT_REGULAR, 21.0f, DWRITE_TEXT_ALIGNMENT_LEADING, false, true},
    {L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 23.0f, DWRITE_TEXT_ALIGNMENT_LEADING, false, false},
    {L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 32.0f, DWRITE_TEXT_ALIGNMENT_LEADING, true, false},
    {L"Segoe UI", DWRITE_FONT_WEIGHT_REGULAR, 26.0f, DWRITE_TEXT_ALIGNMENT_LEADING, true, false},
    {L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 26.0f, DWRITE_TEXT_ALIGNMENT_LEADING, true, false},
    {L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 23.0f, DWRITE_TEXT_ALIGNMENT_LEADING, true, false},
    {L"Segoe UI", DWRITE_FONT_WEIGHT_REGULAR, 21.0f, DWRITE_TEXT_ALIGNMENT_LEADING, true, false},
    {L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 16.0f, DWRITE_TEXT_ALIGNMENT_LEADING, true, false},
    {L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 22.0f, DWRITE_TEXT_ALIGNMENT_TRAILING, true, false},
    {L"Segoe UI", DWRITE_FONT_WEIGHT_REGULAR, 21.0f, DWRITE_TEXT_ALIGNMENT_CENTER, true, false},
    {L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 34.0f, DWRITE_TEXT_ALIGNMENT_CENTER, true, false},
    {L"Segoe MDL2 Assets", DWRITE_FONT_WEIGHT_REGULAR, 22.0f, DWRITE_TEXT_ALIGNMENT_CENTER, true, false},
    {L"Segoe MDL2 Assets", DWRITE_FONT_WEIGHT_REGULAR, 34.0f, DWRITE_TEXT_ALIGNMENT_CENTER, true, false},
};
static_assert(sizeof(kFonts) / sizeof(kFonts[0]) == static_cast<size_t>(Font::Count), "one spec per font");

D2D1_RECT_F ToD2d(Rect r)
{
    return D2D1::RectF(r.left, r.top, r.right, r.bottom);
}

D2D1_COLOR_F Colour(uint32_t rgb, float alpha)
{
    return D2D1::ColorF(rgb, alpha);
}

void LogResult(std::string &log, const char *what, HRESULT result)
{
    char buffer[96] = {};
    sprintf_s(buffer, sizeof(buffer), "%s hr=0x%08lx ", what, static_cast<unsigned long>(result));
    log += buffer;
}

D2D1_RECT_F CoverSource(D2D1_SIZE_F source, Rect dest)
{
    const float destWidth = dest.right - dest.left;
    const float destHeight = dest.bottom - dest.top;
    float cropWidth = source.width;
    float cropHeight = source.height;
    if (source.width / source.height > destWidth / destHeight)
    {
        cropWidth = source.height * destWidth / destHeight;
    }
    else
    {
        cropHeight = source.width * destHeight / destWidth;
    }
    const float x = (source.width - cropWidth) * 0.5f;
    const float y = (source.height - cropHeight) * 0.5f;
    return D2D1::RectF(x, y, x + cropWidth, y + cropHeight);
}
}

struct Ui::State
{
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> context;
    ComPtr<ID2D1Bitmap1> target;
    ComPtr<ID3D11Device> d3dOwned;
    ComPtr<IDWriteFactory> write;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IDWriteTextFormat> formats[static_cast<size_t>(Font::Count)];
    ComPtr<ID2D1SolidColorBrush> brush;
    std::map<std::wstring, ComPtr<ID2D1Bitmap1>> pictures;
    std::map<int64_t, std::pair<const void *, ComPtr<ID2D1Bitmap1>>> thumbs;
    Palette palette;
    float width = 0.0f;
    float height = 0.0f;

    bool Shared(std::string &log)
    {
        D2D1_FACTORY_OPTIONS options = {};
        HRESULT result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                                           reinterpret_cast<void **>(factory.ReleaseAndGetAddressOf()));
        if (FAILED(result))
        {
            LogResult(log, "d2d_factory", result);
            return false;
        }

        result = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                     reinterpret_cast<IUnknown **>(write.ReleaseAndGetAddressOf()));
        if (FAILED(result))
        {
            LogResult(log, "dwrite_factory", result);
            return false;
        }

        result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.ReleaseAndGetAddressOf()));
        if (FAILED(result))
        {
            LogResult(log, "wic_factory", result);
            return false;
        }

        for (size_t at = 0; at < static_cast<size_t>(Font::Count); at++)
        {
            const FontSpec &spec = kFonts[at];
            result = write->CreateTextFormat(spec.family, nullptr, spec.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                             spec.size, L"en-us", formats[at].ReleaseAndGetAddressOf());
            if (FAILED(result))
            {
                LogResult(log, "text_format", result);
                return false;
            }

            formats[at]->SetTextAlignment(spec.align);
            formats[at]->SetParagraphAlignment(spec.centreVertically ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            formats[at]->SetWordWrapping(spec.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
            // each format needs its own ellipsis sign, the sign takes its glyphs from the format
            const bool icon = static_cast<Font>(at) == Font::Icon || static_cast<Font>(at) == Font::IconLarge;
            if (!spec.wrap && !icon)
            {
                ComPtr<IDWriteInlineObject> ellipsis;
                write->CreateEllipsisTrimmingSign(formats[at].Get(), ellipsis.GetAddressOf());
                const DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                formats[at]->SetTrimming(&trimming, ellipsis.Get());
            }
        }
        return true;
    }

    bool Context(IDXGIDevice *dxgi, std::string &log)
    {
        HRESULT result = factory->CreateDevice(dxgi, device.ReleaseAndGetAddressOf());
        if (SUCCEEDED(result))
        {
            result = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, context.ReleaseAndGetAddressOf());
        }
        if (SUCCEEDED(result))
        {
            result = context->CreateSolidColorBrush(Colour(0xFFFFFF, 1.0f), brush.ReleaseAndGetAddressOf());
        }
        if (FAILED(result))
        {
            LogResult(log, "d2d_context", result);
            return false;
        }

        // grayscale stays clean on a tv, cleartype fringes show at sofa distance
        context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        return true;
    }

    ID2D1SolidColorBrush *Brush(uint32_t rgb, float alpha)
    {
        brush->SetColor(Colour(rgb, alpha));
        return brush.Get();
    }

    ComPtr<ID2D1Bitmap1> Load(const std::wstring &path)
    {
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        ComPtr<ID2D1Bitmap1> bitmap;
        if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf())) ||
            FAILED(decoder->GetFrame(0, frame.GetAddressOf())) ||
            FAILED(wic->CreateFormatConverter(converter.GetAddressOf())) ||
            FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
            FAILED(context->CreateBitmapFromWicBitmap(converter.Get(), nullptr, bitmap.GetAddressOf())))
        {
            return nullptr;
        }
        return bitmap;
    }

    // the blur is baked once on a second context, so a frame never switches target mid draw
    ComPtr<ID2D1Bitmap1> Bake(ID2D1Bitmap1 *source, float bakeWidth, float bakeHeight, float blur)
    {
        ComPtr<ID2D1DeviceContext> baker;
        if (FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, baker.GetAddressOf())))
        {
            return nullptr;
        }

        const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(bakeWidth), static_cast<UINT32>(bakeHeight));
        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1Bitmap1> scaled;
        ComPtr<ID2D1Bitmap1> blurred;
        if (FAILED(baker->CreateBitmap(size, nullptr, 0, properties, scaled.GetAddressOf())) ||
            FAILED(baker->CreateBitmap(size, nullptr, 0, properties, blurred.GetAddressOf())))
        {
            return nullptr;
        }

        const Rect full = R(0.0f, 0.0f, bakeWidth, bakeHeight);
        const D2D1_RECT_F crop = CoverSource(source->GetSize(), full);
        baker->SetTarget(scaled.Get());
        baker->BeginDraw();
        baker->Clear(Colour(palette.bg, 1.0f));
        baker->DrawBitmap(source, ToD2d(full), 1.0f, D2D1_INTERPOLATION_MODE_LINEAR, &crop);
        if (FAILED(baker->EndDraw()))
        {
            return nullptr;
        }

        ComPtr<ID2D1Effect> effect;
        if (FAILED(baker->CreateEffect(CLSID_D2D1GaussianBlur, effect.GetAddressOf())))
        {
            return scaled;
        }
        effect->SetInput(0, scaled.Get());
        effect->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, blur);
        effect->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
        effect->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_QUALITY);

        baker->SetTarget(blurred.Get());
        baker->BeginDraw();
        baker->DrawImage(effect.Get());
        if (FAILED(baker->EndDraw()))
        {
            return scaled;
        }
        baker->SetTarget(nullptr);
        return blurred;
    }
};

Ui::Ui() : state_(std::make_unique<State>())
{
}

Ui::~Ui()
{
    Shutdown();
}

bool Ui::AttachSwapchain(ID3D11Device *device, IDXGISwapChain1 *swapchain, std::string &log)
{
    if (device == nullptr || swapchain == nullptr || !state_->Shared(log))
    {
        return false;
    }

    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(dxgi.GetAddressOf()))) || !state_->Context(dxgi.Get(), log))
    {
        return false;
    }

    ComPtr<IDXGISurface> surface;
    HRESULT result = swapchain->GetBuffer(0, IID_PPV_ARGS(surface.GetAddressOf()));
    if (SUCCEEDED(result))
    {
        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        result = state_->context->CreateBitmapFromDxgiSurface(surface.Get(), &properties, state_->target.ReleaseAndGetAddressOf());
    }
    if (FAILED(result))
    {
        LogResult(log, "d2d_target", result);
        return false;
    }

    const D2D1_SIZE_F size = state_->target->GetSize();
    state_->width = size.width;
    state_->height = size.height;
    state_->context->SetTarget(state_->target.Get());
    return true;
}

bool Ui::CreateOffscreen(int width, int height, std::string &log)
{
    if (!state_->Shared(log))
    {
        return false;
    }

    const D3D_DRIVER_TYPE drivers[] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
    HRESULT result = E_FAIL;
    for (const D3D_DRIVER_TYPE driver : drivers)
    {
        result = D3D11CreateDevice(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                   state_->d3dOwned.ReleaseAndGetAddressOf(), nullptr, nullptr);
        if (SUCCEEDED(result))
        {
            break;
        }
    }
    if (FAILED(result))
    {
        LogResult(log, "d3d_device", result);
        return false;
    }

    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(state_->d3dOwned.As(&dxgi)) || !state_->Context(dxgi.Get(), log))
    {
        return false;
    }

    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    result = state_->context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, properties, state_->target.ReleaseAndGetAddressOf());
    if (FAILED(result))
    {
        LogResult(log, "offscreen_target", result);
        return false;
    }

    state_->width = static_cast<float>(width);
    state_->height = static_cast<float>(height);
    state_->context->SetTarget(state_->target.Get());
    return true;
}

void Ui::Shutdown()
{
    if (state_->context)
    {
        state_->context->SetTarget(nullptr);
        state_->context->Flush();
    }
    state_->thumbs.clear();
    state_->pictures.clear();
    state_->brush.Reset();
    state_->target.Reset();
    state_->context.Reset();
    state_->device.Reset();
    state_->d3dOwned.Reset();
}

bool Ui::Begin()
{
    if (!state_->context || !state_->target)
    {
        return false;
    }
    state_->context->BeginDraw();
    return true;
}

bool Ui::End()
{
    return SUCCEEDED(state_->context->EndDraw());
}

bool Ui::SavePng(const std::wstring &path)
{
    const D2D1_SIZE_U size = state_->target->GetPixelSize();
    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    ComPtr<ID2D1Bitmap1> readable;
    if (FAILED(state_->context->CreateBitmap(size, nullptr, 0, properties, readable.GetAddressOf())) ||
        FAILED(readable->CopyFromBitmap(nullptr, state_->target.Get(), nullptr)))
    {
        return false;
    }

    D2D1_MAPPED_RECT mapped = {};
    if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped)))
    {
        return false;
    }

    ComPtr<IWICBitmap> pixels;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    // the png encoder picks its own format, WriteSource converts where WritePixels would smear
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGR;
    HRESULT result = state_->wic->CreateBitmapFromMemory(size.width, size.height, GUID_WICPixelFormat32bppBGR, mapped.pitch,
                                                         mapped.pitch * size.height, mapped.bits, pixels.GetAddressOf());
    if (SUCCEEDED(result)) result = state_->wic->CreateStream(stream.GetAddressOf());
    if (SUCCEEDED(result)) result = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(result)) result = state_->wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
    if (SUCCEEDED(result)) result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(result)) result = encoder->CreateNewFrame(frame.GetAddressOf(), nullptr);
    if (SUCCEEDED(result)) result = frame->Initialize(nullptr);
    if (SUCCEEDED(result)) result = frame->SetSize(size.width, size.height);
    if (SUCCEEDED(result)) result = frame->SetPixelFormat(&format);
    if (SUCCEEDED(result)) result = frame->WriteSource(pixels.Get(), nullptr);
    if (SUCCEEDED(result)) result = frame->Commit();
    if (SUCCEEDED(result)) result = encoder->Commit();
    readable->Unmap();
    return SUCCEEDED(result);
}

float Ui::Width() const
{
    return state_->width;
}

float Ui::Height() const
{
    return state_->height;
}

const Palette &Ui::Colours() const
{
    return state_->palette;
}

void Ui::Clear(uint32_t rgb)
{
    state_->context->Clear(Colour(rgb, 1.0f));
}

void Ui::Fill(Rect r, uint32_t rgb, float alpha)
{
    state_->context->FillRectangle(ToD2d(r), state_->Brush(rgb, alpha));
}

void Ui::FillRound(Rect r, float radius, uint32_t rgb, float alpha)
{
    state_->context->FillRoundedRectangle(D2D1::RoundedRect(ToD2d(r), radius, radius), state_->Brush(rgb, alpha));
}

void Ui::StrokeRound(Rect r, float radius, uint32_t rgb, float alpha, float width)
{
    state_->context->DrawRoundedRectangle(D2D1::RoundedRect(ToD2d(r), radius, radius), state_->Brush(rgb, alpha), width);
}

void Ui::Circle(float cx, float cy, float radius, uint32_t rgb, float alpha)
{
    state_->context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), state_->Brush(rgb, alpha));
}

void Ui::Gradient(Rect r, uint32_t rgb, float alphaStart, float alphaEnd, bool vertical, float midPos, float midAlpha)
{
    const D2D1_GRADIENT_STOP stops[] = {
        {0.0f, Colour(rgb, alphaStart)},
        {midPos, Colour(rgb, midAlpha >= 0.0f ? midAlpha : (alphaStart + alphaEnd) * 0.5f)},
        {1.0f, Colour(rgb, alphaEnd)},
    };
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> gradient;
    const D2D1_POINT_2F from = D2D1::Point2F(r.left, r.top);
    const D2D1_POINT_2F to = vertical ? D2D1::Point2F(r.left, r.bottom) : D2D1::Point2F(r.right, r.top);
    if (SUCCEEDED(state_->context->CreateGradientStopCollection(stops, 3, collection.GetAddressOf())) &&
        SUCCEEDED(state_->context->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(from, to), collection.Get(), gradient.GetAddressOf())))
    {
        state_->context->FillRectangle(ToD2d(r), gradient.Get());
    }
}

float Ui::Text(const std::string &text, Font font, Rect r, uint32_t rgb, float alpha)
{
    const float width = r.right - r.left;
    const float height = r.bottom - r.top;
    if (text.empty() || width <= 0.0f || height <= 0.0f)
    {
        return 0.0f;
    }

    const std::wstring wide = Widen(text);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(state_->write->CreateTextLayout(wide.c_str(), static_cast<UINT32>(wide.size()),
                                               state_->formats[static_cast<size_t>(font)].Get(), width, height, layout.GetAddressOf())))
    {
        return 0.0f;
    }

    DWRITE_TEXT_METRICS metrics = {};
    layout->GetMetrics(&metrics);
    state_->context->PushAxisAlignedClip(ToD2d(r), D2D1_ANTIALIAS_MODE_ALIASED);
    state_->context->DrawTextLayout(D2D1::Point2F(r.left, r.top), layout.Get(), state_->Brush(rgb, alpha));
    state_->context->PopAxisAlignedClip();
    return (std::min)(metrics.height, height);
}

float Ui::TextHeight(const std::string &text, Font font, float width)
{
    if (text.empty())
    {
        return 0.0f;
    }

    const std::wstring wide = Widen(text);
    ComPtr<IDWriteTextLayout> layout;
    DWRITE_TEXT_METRICS metrics = {};
    if (FAILED(state_->write->CreateTextLayout(wide.c_str(), static_cast<UINT32>(wide.size()),
                                               state_->formats[static_cast<size_t>(font)].Get(), width, 4096.0f, layout.GetAddressOf())) ||
        FAILED(layout->GetMetrics(&metrics)))
    {
        return 0.0f;
    }
    return metrics.height;
}

float Ui::TextWidth(const std::string &text, Font font)
{
    if (text.empty())
    {
        return 0.0f;
    }

    const std::wstring wide = Widen(text);
    ComPtr<IDWriteTextLayout> layout;
    DWRITE_TEXT_METRICS metrics = {};
    if (FAILED(state_->write->CreateTextLayout(wide.c_str(), static_cast<UINT32>(wide.size()),
                                               state_->formats[static_cast<size_t>(font)].Get(), 4096.0f, 200.0f, layout.GetAddressOf())))
    {
        return 0.0f;
    }
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}

void Ui::Icon(const wchar_t *glyph, Rect r, uint32_t rgb, bool large)
{
    if (glyph == nullptr || *glyph == L'\0')
    {
        return;
    }
    const Font font = large ? Font::IconLarge : Font::Icon;
    state_->context->DrawText(glyph, static_cast<UINT32>(wcslen(glyph)), state_->formats[static_cast<size_t>(font)].Get(), ToD2d(r),
                              state_->Brush(rgb, 1.0f));
}

bool Ui::Picture(const std::wstring &path, Rect r, float opacity, float blur)
{
    const float width = r.right - r.left;
    const float height = r.bottom - r.top;
    if (path.empty() || width < 1.0f || height < 1.0f)
    {
        return false;
    }

    wchar_t suffix[64] = {};
    swprintf_s(suffix, L"|%.0fx%.0f|%.1f", width, height, blur);
    const std::wstring key = path + suffix;

    auto found = state_->pictures.find(key);
    if (found == state_->pictures.end())
    {
        ComPtr<ID2D1Bitmap1> source = state_->Load(path);
        ComPtr<ID2D1Bitmap1> ready = (source && blur > 0.0f) ? state_->Bake(source.Get(), width, height, blur) : source;
        // a missing file is remembered too, so a bad path is not reopened sixty times a second
        found = state_->pictures.emplace(key, ready).first;
    }
    if (!found->second)
    {
        return false;
    }

    const D2D1_RECT_F crop = CoverSource(found->second->GetSize(), r);
    state_->context->DrawBitmap(found->second.Get(), ToD2d(r), opacity, D2D1_INTERPOLATION_MODE_LINEAR, &crop);
    return true;
}

bool Ui::Logo(const std::wstring &path, Rect r)
{
    auto found = state_->pictures.find(path);
    if (found == state_->pictures.end())
    {
        found = state_->pictures.emplace(path, state_->Load(path)).first;
    }
    if (!found->second)
    {
        return false;
    }

    const D2D1_SIZE_F size = found->second->GetSize();
    const float scale = (std::min)((r.right - r.left) / size.width, (r.bottom - r.top) / size.height);
    const float w = size.width * scale;
    const float h = size.height * scale;
    const float x = r.left + ((r.right - r.left) - w) * 0.5f;
    const float y = r.top + ((r.bottom - r.top) - h) * 0.5f;
    state_->context->DrawBitmap(found->second.Get(), D2D1::RectF(x, y, x + w, y + h), 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    return true;
}

void Ui::Thumbnail(int64_t key, const Bitmap &bitmap, Rect r)
{
    if (bitmap.bgra.empty() || bitmap.width <= 0 || bitmap.height <= 0)
    {
        return;
    }

    auto &slot = state_->thumbs[key];
    if (slot.first != bitmap.bgra.data() || !slot.second)
    {
        // d2d wants premultiplied alpha, the decoder hands back straight alpha
        std::vector<uint32_t> premultiplied(bitmap.bgra.size());
        for (size_t at = 0; at < bitmap.bgra.size(); at++)
        {
            const uint32_t pixel = bitmap.bgra[at];
            const uint32_t a = pixel >> 24;
            const uint32_t b = ((pixel & 0xFF) * a) / 255;
            const uint32_t g = (((pixel >> 8) & 0xFF) * a) / 255;
            const uint32_t red = (((pixel >> 16) & 0xFF) * a) / 255;
            premultiplied[at] = (a << 24) | (red << 16) | (g << 8) | b;
        }

        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        slot.first = bitmap.bgra.data();
        state_->context->CreateBitmap(D2D1::SizeU(bitmap.width, bitmap.height), premultiplied.data(), bitmap.width * 4, properties,
                                      slot.second.ReleaseAndGetAddressOf());
    }
    if (!slot.second)
    {
        return;
    }

    // drawn through a bitmap brush so the art gets the same rounded corners as its tile
    ComPtr<ID2D1BitmapBrush1> art;
    if (FAILED(state_->context->CreateBitmapBrush(slot.second.Get(), art.GetAddressOf())))
    {
        return;
    }
    const D2D1_SIZE_F size = slot.second->GetSize();
    art->SetInterpolationMode1(D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    art->SetTransform(D2D1::Matrix3x2F::Scale((r.right - r.left) / size.width, (r.bottom - r.top) / size.height) *
                      D2D1::Matrix3x2F::Translation(r.left, r.top));
    state_->context->FillRoundedRectangle(D2D1::RoundedRect(ToD2d(r), 10.0f, 10.0f), art.Get());
}

void Ui::ForgetThumbnails()
{
    state_->thumbs.clear();
}

void Ui::Qr(const std::string &text, Rect r)
{
    if (text.empty())
    {
        return;
    }

    std::vector<uint8_t> code(qrcodegen_BUFFER_LEN_FOR_VERSION(kQrMaxVersion));
    std::vector<uint8_t> scratch(qrcodegen_BUFFER_LEN_FOR_VERSION(kQrMaxVersion));
    if (!qrcodegen_encodeText(text.c_str(), scratch.data(), code.data(), qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, kQrMaxVersion,
                              qrcodegen_Mask_AUTO, true))
    {
        return;
    }

    const int size = qrcodegen_getSize(code.data());
    const float side = (std::min)(r.right - r.left, r.bottom - r.top);
    // whole pixel modules, a blurred edge between modules can stop a phone reading it
    const float module = std::floor(side / static_cast<float>(size));
    const float x0 = r.left + std::floor((side - module * size) * 0.5f);
    const float y0 = r.top + std::floor((side - module * size) * 0.5f);
    ID2D1SolidColorBrush *ink = state_->Brush(0x000000, 1.0f);
    state_->context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    for (int row = 0; row < size; row++)
    {
        for (int column = 0; column < size; column++)
        {
            if (qrcodegen_getModule(code.data(), column, row))
            {
                const float x = x0 + column * module;
                const float y = y0 + row * module;
                state_->context->FillRectangle(D2D1::RectF(x, y, x + module, y + module), ink);
            }
        }
    }
    state_->context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
}
}
