#include "launcher_screens.h"

#include "launcher_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace stardew_uwp
{
const char *const kKeyboardRows[4] = {
    "abcdefghij",
    "klmnopqrst",
    "uvwxyz0123",
    "456789-'.&",
};

KeyAction KeyActionAt(int index)
{
    if (index < 40)
    {
        return KeyAction::Character;
    }
    const int cell = index - 40;
    if (cell < 4)
    {
        return KeyAction::Space;
    }
    return cell < 7 ? KeyAction::Delete : KeyAction::Search;
}

bool LoadBearing(const InstalledMod &mod)
{
    return mod.uniqueId.compare(0, 6, "SMAPI.") == 0 || mod.name == "Xbox Paths";
}

namespace
{
// segoe mdl2 glyphs, same as Bandit Launcher
constexpr const wchar_t *kIconPlay = L"\uE768";
constexpr const wchar_t *kIconMods = L"\uE74C";
constexpr const wchar_t *kIconDownload = L"\uE896";
constexpr const wchar_t *kIconSignOut = L"\uE7E8";
constexpr const wchar_t *kIconBack = L"\uE72B";
constexpr const wchar_t *kIconSearch = L"\uE721";
constexpr const wchar_t *kIconPopular = L"\uE734";
constexpr const wchar_t *kIconInstalled = L"\uE8F1";
constexpr const wchar_t *kIconDelete = L"\uE74D";
constexpr const wchar_t *kIconKey = L"\uE8D7";
constexpr const wchar_t *kIconPhone = L"\uE8EA";
constexpr const wchar_t *kIconWarning = L"\uE7BA";

const std::vector<InstalledMod> kNoInstalled;
const std::vector<CatalogueMod> kNoCatalogue;
const std::map<int64_t, Bitmap> kNoThumbs;

std::string Thousands(uint64_t value)
{
    std::string digits = std::to_string(value);
    for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3)
    {
        digits.insert(static_cast<std::size_t>(at), ",");
    }
    return digits;
}

std::string Bytes(uint64_t value)
{
    static const char *units[] = {"B", "KB", "MB", "GB"};
    double amount = static_cast<double>(value);
    int unit = 0;
    while (amount >= 1024.0 && unit < 3)
    {
        amount /= 1024.0;
        unit += 1;
    }
    char buffer[32] = {};
    snprintf(buffer, sizeof(buffer), unit == 0 ? "%.0f %s" : "%.1f %s", amount, units[unit]);
    return buffer;
}

class Painter
{
public:
    Painter(Ui &ui, const LauncherView &view) : ui_(ui), view_(view), c_(ui.Colours())
    {
        // inside the tv title safe area, overscan clips anything closer to the edge
        L_ = std::floor(ui.Width() * 0.05f);
        T_ = std::floor(ui.Height() * 0.065f);
        R_ = ui.Width() - L_;
        B_ = ui.Height() - T_;
    }

    void Draw()
    {
        ui_.Clear(c_.bg);
        switch (view_.screen)
        {
        case Screen::Menu: Menu(); break;
        case Screen::Steam: Steam(); break;
        case Screen::Mods: Mods(); break;
        case Screen::Browse: Browse(); break;
        case Screen::Keyboard: Keyboard(); break;
        case Screen::Installed: Installed(); break;
        case Screen::Confirm: Confirm(); break;
        case Screen::Pairing: Pairing(); break;
        }
    }

private:
    const std::vector<InstalledMod> &InstalledList() const
    {
        return view_.installed ? *view_.installed : kNoInstalled;
    }

    const std::vector<CatalogueMod> &Catalogue() const
    {
        return view_.catalogue ? *view_.catalogue : kNoCatalogue;
    }

    bool Backdrop()
    {
        const Rect full = R(0.0f, 0.0f, ui_.Width(), ui_.Height());
        const int total = static_cast<int>(view_.backdrops.size());
        if (total == 0)
        {
            return false;
        }

        const double t = view_.seconds / 6.0;
        const long long base = static_cast<long long>(std::floor(t));
        const int index = static_cast<int>(((base % total) + total) % total);
        const double frac = t - static_cast<double>(base);
        const double fade = frac > 0.82 ? (frac - 0.82) / 0.18 : 0.0;
        const float eased = static_cast<float>(fade * fade * (3.0 - 2.0 * fade));

        const bool drawn = ui_.Picture(view_.backdrops[static_cast<std::size_t>(index)], full, 1.0f, 14.0f);
        if (eased > 0.0f && total > 1)
        {
            ui_.Picture(view_.backdrops[static_cast<std::size_t>((index + 1) % total)], full, eased, 14.0f);
        }
        return drawn;
    }

    void Dimmed(float cover)
    {
        Backdrop();
        ui_.Fill(R(0.0f, 0.0f, ui_.Width(), ui_.Height()), c_.bg, cover);
    }

    void Brand(const std::string &label, uint32_t ink)
    {
        float x = L_;
        if (!view_.logoPath.empty() && ui_.Logo(view_.logoPath, R(L_, T_, L_ + 44.0f, T_ + 44.0f)))
        {
            x = L_ + 58.0f;
        }
        ui_.Text(label, Font::Overline, R(x, T_, x + 700.0f, T_ + 44.0f), ink);
    }

    Rect BackChip(float x, float top)
    {
        const Rect chip = R(x, top, x + 50.0f, top + 50.0f);
        ui_.FillRound(chip, 25.0f, c_.raised, 0.96f);
        ui_.Icon(kIconBack, chip, c_.text);
        return chip;
    }

    void Header(const std::string &title)
    {
        const Rect back = BackChip(L_, T_);
        ui_.Text(title, Font::Title, R(back.right + 20.0f, T_ - 4.0f, R_, T_ + 54.0f), c_.text);
    }

    void Footer(const std::string &hint, uint32_t ink = 0)
    {
        ui_.Fill(R(L_, B_ - 46.0f, R_, B_ - 45.0f), c_.text, 0.10f);
        ui_.Text(hint, Font::SmallMid, R(L_, B_ - 38.0f, R_, B_), ink ? ink : c_.muted);
    }

    void FocusRing(Rect r, float radius)
    {
        const float off = 5.0f;
        ui_.StrokeRound(R(r.left - off, r.top - off, r.right + off, r.bottom + off), radius + off, c_.text, 1.0f, 3.0f);
    }

    void Row(Rect r, const wchar_t *glyph, const std::string &label, bool focused, bool active, bool large = false,
             uint32_t idle = 0, bool enabled = true)
    {
        uint32_t ink = idle ? idle : c_.text;
        if (!enabled)
        {
            ink = c_.muted;
        }
        if (focused)
        {
            ui_.FillRound(r, 12.0f, c_.text);
            ink = c_.onLight;
        }
        else if (active)
        {
            ui_.FillRound(r, 12.0f, c_.raised, 0.96f);
            ui_.FillRound(R(r.left, r.top + 14.0f, r.left + 4.0f, r.bottom - 14.0f), 2.0f, c_.accent);
            ink = c_.accent;
        }
        ui_.Icon(glyph, R(r.left + 18.0f, r.top, r.left + 52.0f, r.bottom), ink, large);
        const Font font = large ? Font::RowLarge : ((focused || active) ? Font::RowStrong : Font::Row);
        ui_.Text(label, font, R(r.left + 68.0f, r.top, r.right - 16.0f, r.bottom), ink);
    }

    enum class Kind
    {
        Neutral,
        Primary,
        Danger,
    };

    void Button(Rect r, const wchar_t *glyph, const std::string &label, bool focused, Kind kind)
    {
        uint32_t ink = c_.text;
        if (kind == Kind::Primary)
        {
            ui_.FillRound(r, 12.0f, c_.accent);
            ink = c_.onLight;
            if (focused)
            {
                FocusRing(r, 12.0f);
            }
        }
        else if (kind == Kind::Danger)
        {
            ui_.FillRound(r, 12.0f, c_.danger);
            ink = c_.onLight;
            if (focused)
            {
                FocusRing(r, 12.0f);
            }
        }
        else if (focused)
        {
            ui_.FillRound(r, 12.0f, c_.text);
            ink = c_.onLight;
        }
        else
        {
            ui_.FillRound(r, 12.0f, c_.raised, 0.96f);
        }

        float textLeft = r.left + 20.0f;
        if (glyph != nullptr)
        {
            ui_.Icon(glyph, R(r.left + 14.0f, r.top, r.left + 44.0f, r.bottom), ink);
            textLeft = r.left + 50.0f;
        }
        ui_.Text(label, Font::Button, R(textLeft, r.top, r.right - 10.0f, r.bottom), ink);
    }

    void Bar(float left, float right, float top, uint64_t done, uint64_t total, uint32_t ink)
    {
        const float height = 8.0f;
        ui_.FillRound(R(left, top, right, top + height), height * 0.5f, c_.text, 0.16f);
        if (total != 0 && done != 0)
        {
            const float fraction = (std::min)(1.0f, static_cast<float>(static_cast<double>(done) / static_cast<double>(total)));
            ui_.FillRound(R(left, top, left + (std::max)(height, (right - left) * fraction), top + height), height * 0.5f, ink);
        }
    }

    void QrCard(const std::string &url, float columnLeft)
    {
        const float side = (std::min)((R_ - columnLeft) * 0.62f, (B_ - T_) * 0.56f);
        const float pad = 26.0f;
        const float card = side + pad * 2.0f;
        const float left = columnLeft + ((R_ - columnLeft) - card) * 0.5f;
        const float top = T_ + ((B_ - T_) - card) * 0.44f;
        const Rect box = R(left, top, left + card, top + card);
        ui_.FillRound(box, 20.0f, 0xFFFFFF);
        if (url.empty())
        {
            ui_.Text("Waiting for Steam", Font::Center, box, c_.onLight, 0.6f);
        }
        else
        {
            ui_.Qr(url, R(box.left + pad, box.top + pad, box.right - pad, box.bottom - pad));
        }
        ui_.Text("Scan QR", Font::Center, R(box.left, box.bottom + 18.0f, box.right, box.bottom + 54.0f), c_.muted);
    }

    void Menu()
    {
        const float w = ui_.Width();
        const float h = ui_.Height();
        Backdrop();
        ui_.Gradient(R(0.0f, 0.0f, w * 0.80f, h), c_.bg, 0.96f, 0.0f, false, 0.42f, 0.86f);
        ui_.Gradient(R(0.0f, h * 0.72f, w, h), c_.bg, 0.0f, 0.55f, true);

        const float menuRight = L_ + 520.0f;
        Brand("STARDEW VALLEY UWP", c_.muted);

        const std::string title = view_.steam.accountName.empty() ? std::string("Stardew Valley") : view_.steam.accountName;
        ui_.Text(title, Font::Display, R(L_ - 3.0f, T_ + 64.0f, R_ - 200.0f, T_ + 148.0f), c_.text);

        const std::size_t mods = InstalledList().size();
        const std::string status = view_.gameInstalled ? "Ready to play" : "The game is not installed yet";
        const std::string detail = view_.gameInstalled
                                       ? ("SMAPI with " + std::to_string(mods) + (mods == 1 ? " mod" : " mods"))
                                       : "Choose Download game to get your copy from Steam";
        float y = T_ + 158.0f;
        y += ui_.Text(status, Font::Lead, R(L_, y, menuRight + 160.0f, y + 68.0f), view_.gameInstalled ? c_.text : c_.accent) + 4.0f;
        ui_.Text(detail, Font::Small, R(L_, y, menuRight + 160.0f, y + 60.0f), c_.muted);

        const char *labels[] = {"Play", "Mods", view_.gameInstalled ? "Update game" : "Download game", "Sign out of Steam"};
        const wchar_t *icons[] = {kIconPlay, kIconMods, kIconDownload, kIconSignOut};

        float rowY = T_ + 276.0f;
        for (int i = 0; i < kMenuItems; i++)
        {
            const bool play = i == 0;
            const float rowH = play ? 84.0f : 62.0f;
            const Rect row = R(L_, rowY, menuRight, rowY + rowH);
            const bool selected = i == view_.menuIndex;
            if (play && !selected)
            {
                ui_.FillRound(row, 12.0f, c_.accent, 0.16f);
            }
            Row(row, icons[i], labels[i], selected, false, play, play ? c_.accent : 0, !play || view_.gameInstalled);
            // a dot on the download row while there is no game, the one thing a new player has to do
            if (i == 2 && !view_.gameInstalled)
            {
                ui_.Circle(row.right - 30.0f, (row.top + row.bottom) * 0.5f, 7.0f, selected ? c_.onLight : c_.accent);
            }
            rowY += rowH + (play ? 14.0f : 4.0f);
        }

        if (!view_.notice.empty())
        {
            ui_.Text(view_.notice, Font::Lead, R(L_, rowY + 30.0f, R_, rowY + 70.0f), c_.accent);
        }
    }

    void Steam()
    {
        const SteamView &steam = view_.steam;
        if (!steam.signedIn)
        {
            Dimmed(0.84f);
            Brand("STARDEW VALLEY UWP", c_.muted);

            const float column = L_ + (R_ - L_) * 0.52f;
            ui_.Text("Sign in with Steam", Font::Display, R(L_ - 3.0f, T_ + 96.0f, column, T_ + 180.0f), c_.text);

            const char *steps[] = {"Open the Steam app on your phone", "Scan the code and approve the sign in"};
            float y = T_ + 240.0f;
            for (int i = 0; i < 2; i++)
            {
                const Rect badge = R(L_, y, L_ + 44.0f, y + 44.0f);
                ui_.FillRound(badge, 22.0f, c_.raised, 0.96f);
                ui_.Text(std::to_string(i + 1), Font::Center, badge, c_.accent);
                ui_.Text(steps[i], Font::Row, R(L_ + 64.0f, y, column - 40.0f, y + 44.0f), c_.text);
                y += 62.0f;
            }

            y += 30.0f;
            ui_.Text("This downloads your own copy of Stardew Valley. No game files ship with this app.", Font::Small,
                     R(L_, y, column - 40.0f, y + 80.0f), c_.muted);
            if (!steam.message.empty())
            {
                ui_.Text(steam.message, Font::Small, R(L_, B_ - 90.0f, column - 40.0f, B_ - 50.0f), c_.muted);
            }

            QrCard(steam.challengeUrl, column);
            Footer("Press B to go back");
            return;
        }

        const float w = ui_.Width();
        const float h = ui_.Height();
        Dimmed(0.50f);
        ui_.Gradient(R(0.0f, h * 0.45f, w, h), c_.bg, 0.0f, 0.92f, true);
        Brand(steam.accountName.empty() ? std::string("SIGNED IN TO STEAM") : "SIGNED IN AS " + steam.accountName, c_.text);

        const bool busy = steam.totalBytes > 0 && (steam.installState == "downloading" || steam.installState == "verifying");
        std::string status;
        std::string detail;
        if (busy)
        {
            const int percent = static_cast<int>((steam.transferredBytes * 100ull) / steam.totalBytes);
            status = (steam.installState == "verifying" ? "Checking the game files, " : "Downloading Stardew Valley, ") + std::to_string(percent) + "%";
            detail = Bytes(steam.transferredBytes) + " of " + Bytes(steam.totalBytes);
            if (steam.totalFiles > 0)
            {
                detail += ", " + Thousands(steam.filesVerified) + " of " + Thousands(steam.totalFiles) + " files";
            }
        }
        else if (steam.installState == "installed")
        {
            status = "Stardew Valley is installed";
            detail = Bytes(steam.installedBytes) + " on this console. Press A to check for an update.";
        }
        else if (!steam.installMessage.empty())
        {
            status = steam.installMessage;
        }
        else
        {
            status = "Ready to download Stardew Valley";
            detail = "Press A to download your copy from Steam.";
        }

        const float barTop = B_ - 54.0f;
        const float detailH = detail.empty() ? 0.0f : (std::min)(62.0f, ui_.TextHeight(detail, Font::Small, R_ - L_));
        const float statusH = (std::max)(56.0f, (std::min)(170.0f, ui_.TextHeight(status, Font::Headline, R_ - L_)));
        const float detailTop = barTop - 26.0f - detailH;
        const float statusTop = detailTop - (detailH > 0.0f ? 8.0f : 0.0f) - statusH;
        ui_.Text(status, Font::Headline, R(L_, statusTop, R_, statusTop + statusH), c_.text);
        ui_.Text(detail, Font::Small, R(L_, detailTop, R_, detailTop + detailH), c_.muted);

        Bar(L_, R_, barTop, busy ? steam.transferredBytes : (steam.installState == "installed" ? 1 : 0), busy ? steam.totalBytes : 1, c_.accent);
        ui_.Text(busy ? "Press B to cancel" : "Press A to download, B to go back", Font::SmallMid,
                 R(L_, barTop + 16.0f, R_, barTop + 48.0f), c_.muted);
    }

    // focus sits on the rail on the mods screen, inside a list the rail marks where it came from
    void Rail(int focused, int active)
    {
        const float railRight = L_ + 350.0f;
        const char *labels[] = {"Search", "Popular", "Installed"};
        const wchar_t *icons[] = {kIconSearch, kIconPopular, kIconInstalled};

        const Rect source = R(L_, T_ + 82.0f, railRight, T_ + 132.0f);
        ui_.FillRound(source, 12.0f, c_.surface, 0.94f);
        ui_.FillRound(R(source.left + 4.0f, source.top + 4.0f, source.right - 4.0f, source.bottom - 4.0f), 9.0f, c_.raised, 0.96f);
        ui_.Text("CurseForge", Font::Center, source, c_.accent);

        float y = T_ + 160.0f;
        for (int i = 0; i < kModsItems; i++)
        {
            const Rect row = R(L_, y, railRight, y + 58.0f);
            std::string label = labels[i];
            if (i == 2)
            {
                label += "  " + std::to_string(InstalledList().size());
            }
            const bool live = view_.curseForgeReady || i == 2;
            Row(row, icons[i], label, i == focused, i == active, false, 0, view_.curseForgeConfigured && live);
            y += 64.0f;
        }

        y += 24.0f;
        ui_.Fill(R(L_, y, railRight, y + 1.0f), c_.text, 0.10f);
        y += 26.0f;
        ui_.Text("MODS LOAD FROM", Font::Overline, R(L_ + 4.0f, y, railRight, y + 24.0f), c_.muted);
        ui_.Text("LocalState\\Mods", Font::RowStrong, R(L_ + 4.0f, y + 24.0f, railRight, y + 62.0f), c_.accent);
        ui_.Text("SMAPI picks them up the next time you press Play", Font::Small, R(L_ + 4.0f, y + 70.0f, railRight, y + 140.0f), c_.muted);
    }

    void Status(float left, float right, float top)
    {
        const CurseForgeStatus &status = view_.curseForge;
        if (status.message.empty())
        {
            return;
        }
        const bool failed = status.state == "failed";
        ui_.Text(status.message, Font::SmallMid, R(left, top, right, top + 32.0f), failed ? c_.danger : c_.accent);
        if (status.state == "working" && status.total != 0)
        {
            Bar(left, right, top + 40.0f, status.transferred, status.total, c_.accent);
        }
    }

    void Mods()
    {
        Header("Mods");
        const float panelLeft = L_ + 396.0f;

        if (!view_.curseForgeConfigured)
        {
            Rail(-1, -1);
            SetupPanel(panelLeft);
            Footer("Press A to enter a key, B to go back");
            return;
        }

        Rail(view_.modsIndex, -1);

        const wchar_t *icons[] = {kIconSearch, kIconPopular, kIconInstalled};
        const char *titles[] = {"Search CurseForge", "Popular on CurseForge", "Your installed mods"};
        const char *bodies[] = {
            "Type a name with the on screen keyboard and pick from what comes back. A mod installs straight into "
            "LocalState\\Mods and is ready the next time you play.",
            "The most downloaded Stardew Valley mods right now, eight to a page. Use LB and RB to move between pages.",
            "Everything SMAPI will load, with its version and folder. You can delete a mod from here.",
        };
        const int pick = (std::max)(0, (std::min)(kModsItems - 1, view_.modsIndex));
        const bool hint = !view_.curseForgeReady && pick != 2 && !view_.curseForgeHint.empty();

        // the card is sized to what it says, a tall empty panel reads as something failing to load
        const float textRight = R_ - 120.0f;
        const float bodyH = ui_.TextHeight(bodies[pick], Font::Lead, textRight - (panelLeft + 48.0f));
        const float hintH = hint ? ui_.TextHeight(view_.curseForgeHint, Font::Small, textRight - (panelLeft + 48.0f)) + 28.0f : 0.0f;
        const Rect panel = R(panelLeft, T_ + 82.0f, R_, T_ + 82.0f + 48.0f + 96.0f + 68.0f + bodyH + hintH + 48.0f);
        ui_.FillRound(panel, 16.0f, c_.surface, 0.94f);
        const float x = panel.left + 48.0f;
        float y = panel.top + 48.0f;

        ui_.Circle(x + 32.0f, y + 32.0f, 32.0f, c_.accent, 0.16f);
        ui_.Icon(icons[pick], R(x, y, x + 64.0f, y + 64.0f), c_.accent, true);
        y += 96.0f;
        ui_.Text(titles[pick], Font::Title, R(x, y, panel.right - 48.0f, y + 56.0f), c_.text);
        y += 68.0f;
        y += ui_.Text(bodies[pick], Font::Lead, R(x, y, textRight, y + bodyH + 4.0f), c_.muted);

        if (hint)
        {
            y += 28.0f;
            ui_.Text(view_.curseForgeHint, Font::Small, R(x, y, textRight, y + hintH), c_.danger);
        }

        Status(panelLeft, R_, B_ - 120.0f);
        Footer("Press A to open, B to go back");
    }

    // nobody can act on a missing curseforge.json from a sofa, so the phone flow is the one action here
    void SetupPanel(float left)
    {
        const Rect panel = R(left, T_ + 82.0f, R_, B_ - 80.0f);
        ui_.FillRound(panel, 16.0f, c_.surface, 0.94f);
        const float x = panel.left + 48.0f;
        float y = panel.top + 48.0f;

        ui_.Circle(x + 32.0f, y + 32.0f, 32.0f, c_.accent, 0.16f);
        ui_.Icon(kIconKey, R(x, y, x + 64.0f, y + 64.0f), c_.accent, true);
        y += 96.0f;
        ui_.Text("CurseForge needs a key of your own", Font::Title, R(x, y, panel.right - 48.0f, y + 56.0f), c_.text);
        y += 68.0f;
        ui_.Text("Keys are free from console.curseforge.com. Yours stays on this console and is only sent to CurseForge.", Font::Lead,
                 R(x, y, panel.right - 120.0f, y + 80.0f), c_.muted);
        y += 110.0f;

        Button(R(x, y, x + 470.0f, y + 64.0f), kIconPhone, "Enter the key from your phone", true, Kind::Neutral);
        y += 112.0f;

        ui_.Text("Or upload curseforge.json to LocalState with the Device Portal:", Font::Small, R(x, y, panel.right - 48.0f, y + 32.0f), c_.muted);
        y += 44.0f;
        const Rect code = R(x, y, x + 520.0f, y + 52.0f);
        ui_.FillRound(code, 10.0f, c_.bg, 0.9f);
        ui_.Text("{\"apiKey\": \"your key here\"}", Font::SmallMid, R(code.left + 20.0f, code.top, code.right - 10.0f, code.bottom), c_.text);

        if (!view_.curseForgeHint.empty())
        {
            ui_.Text(view_.curseForgeHint, Font::Small, R(x, panel.bottom - 70.0f, panel.right - 48.0f, panel.bottom - 30.0f), c_.danger);
        }
    }

    void Card(const CatalogueMod &mod, Rect r, bool focused)
    {
        if (focused)
        {
            ui_.FillRound(r, 12.0f, c_.raised, 0.96f);
            FocusRing(r, 12.0f);
        }
        else
        {
            ui_.FillRound(r, 12.0f, c_.surface, 0.94f);
        }

        const float tile = r.bottom - r.top - 32.0f;
        const Rect art = R(r.left + 16.0f, r.top + 16.0f, r.left + 16.0f + tile, r.top + 16.0f + tile);
        const std::map<int64_t, Bitmap> &thumbs = view_.thumbs ? *view_.thumbs : kNoThumbs;
        const auto found = thumbs.find(mod.modId);
        if (found != thumbs.end() && !found->second.bgra.empty())
        {
            ui_.Thumbnail(mod.modId, found->second, art);
        }
        else
        {
            ui_.FillRound(art, 10.0f, 0x000000, 0.45f);
            ui_.Icon(kIconMods, art, c_.muted, true);
        }

        const float x = art.right + 24.0f;
        ui_.Text(mod.name, Font::CardTitle, R(x, r.top + 18.0f, r.right - 20.0f, r.top + 52.0f), c_.text);
        ui_.Text("by " + mod.author, Font::SmallMid, R(x, r.top + 52.0f, r.right - 20.0f, r.top + 84.0f), c_.muted);
        if (mod.distributable)
        {
            ui_.Text(Thousands(static_cast<uint64_t>(mod.downloads)) + " downloads", Font::SmallMid,
                     R(x, r.bottom - 50.0f, r.right - 20.0f, r.bottom - 16.0f), c_.muted);
        }
        else
        {
            ui_.Icon(kIconWarning, R(x - 4.0f, r.bottom - 50.0f, x + 26.0f, r.bottom - 16.0f), c_.danger);
            ui_.Text("The author blocks downloads from other apps", Font::SmallMid, R(x + 32.0f, r.bottom - 50.0f, r.right - 20.0f, r.bottom - 16.0f),
                     c_.danger);
        }
    }

    void Browse()
    {
        Header("Mods");
        const bool searching = !view_.query.empty();
        Rail(-1, searching ? 0 : 1);

        const float left = L_ + 396.0f;
        const Rect bar = R(left, T_, R_, T_ + 52.0f);
        ui_.FillRound(bar, 12.0f, c_.surface, 0.94f);
        ui_.Icon(searching ? kIconSearch : kIconPopular, R(bar.left + 14.0f, bar.top, bar.left + 44.0f, bar.bottom), c_.muted);
        ui_.Text(searching ? ("Results for \"" + view_.query + "\"") : std::string("Popular on CurseForge"), Font::Row,
                 R(bar.left + 54.0f, bar.top, bar.right - 220.0f, bar.bottom), c_.text);
        ui_.Text("Page " + std::to_string(view_.page + 1), Font::Value, R(bar.right - 220.0f, bar.top, bar.right - 22.0f, bar.bottom), c_.muted);

        const std::vector<CatalogueMod> &mods = Catalogue();
        const float gap = 16.0f;
        const float top = T_ + 82.0f;
        const float cardW = (R_ - left - gap) / kCardColumns;
        const float cardH = 156.0f;
        for (std::size_t at = 0; at < mods.size() && at < static_cast<std::size_t>(kCardColumns * kCardRows); at++)
        {
            const int column = static_cast<int>(at) % kCardColumns;
            const int row = static_cast<int>(at) / kCardColumns;
            const float x = left + column * (cardW + gap);
            const float y = top + row * (cardH + gap);
            Card(mods[at], R(x, y, x + cardW, y + cardH), static_cast<int>(at) == view_.browseIndex);
        }

        if (mods.empty())
        {
            ui_.Text(view_.curseForge.state == "working" ? "Asking CurseForge" : "Nothing came back for that", Font::Lead,
                     R(left, top + 10.0f, R_, top + 60.0f), c_.muted);
        }

        Status(left, R_, B_ - 112.0f);
        Footer(view_.curseForge.state == "working" && view_.curseForge.total != 0 ? "Press B to cancel the download"
                                                                                    : "Press A to install, B to go back, LB and RB to change page");
    }

    void Keyboard()
    {
        Header("Search CurseForge");

        const Rect field = R(L_, T_ + 96.0f, R_, T_ + 168.0f);
        ui_.FillRound(field, 14.0f, c_.surface, 0.94f);
        ui_.Icon(kIconSearch, R(field.left + 18.0f, field.top, field.left + 52.0f, field.bottom), c_.muted, true);
        const float textLeft = field.left + 72.0f;
        if (view_.query.empty())
        {
            ui_.Text("Type a mod name", Font::RowLarge, R(textLeft, field.top, field.right - 20.0f, field.bottom), c_.muted);
        }
        else
        {
            ui_.Text(view_.query, Font::RowLarge, R(textLeft, field.top, field.right - 20.0f, field.bottom), c_.text);
        }
        const float caret = textLeft + ui_.TextWidth(view_.query, Font::RowLarge) + 4.0f;
        ui_.Fill(R(caret, field.top + 18.0f, caret + 3.0f, field.bottom - 18.0f), c_.accent);

        const float gap = 12.0f;
        const float keyW = 140.0f;
        const float keyH = 92.0f;
        const float gridW = kKeyColumns * keyW + (kKeyColumns - 1) * gap;
        const float originX = std::floor((ui_.Width() - gridW) * 0.5f);
        const float originY = T_ + 220.0f;

        for (int index = 0; index < 40; index++)
        {
            const int column = index % kKeyColumns;
            const int row = index / kKeyColumns;
            const float x = originX + column * (keyW + gap);
            const float y = originY + row * (keyH + gap);
            const Rect key = R(x, y, x + keyW, y + keyH);
            const bool focused = index == view_.keyIndex;
            ui_.FillRound(key, 12.0f, focused ? c_.text : c_.raised, focused ? 1.0f : 0.96f);
            ui_.Text(std::string(1, kKeyboardRows[row][column]), Font::Key, key, focused ? c_.onLight : c_.text);
        }

        const float actionY = originY + 4 * (keyH + gap);
        const int starts[] = {0, 4, 7};
        const int widths[] = {4, 3, 3};
        const char *labels[] = {"Space", "Delete", "Search"};
        const KeyAction actions[] = {KeyAction::Space, KeyAction::Delete, KeyAction::Search};
        const wchar_t *icons[] = {nullptr, L"\uE750", kIconSearch};
        for (int i = 0; i < 3; i++)
        {
            const float x = originX + starts[i] * (keyW + gap);
            const float right = x + widths[i] * keyW + (widths[i] - 1) * gap;
            const bool focused = view_.keyIndex >= 40 && KeyActionAt(view_.keyIndex) == actions[i];
            Button(R(x, actionY, right, actionY + keyH), icons[i], labels[i], focused, i == 2 ? Kind::Primary : Kind::Neutral);
        }

        Footer("Press A to type, B to go back");
    }

    void Installed()
    {
        const std::vector<InstalledMod> &mods = InstalledList();
        Header("Installed mods");
        ui_.Text(std::to_string(mods.size()) + (mods.size() == 1 ? " mod in LocalState\\Mods" : " mods in LocalState\\Mods"), Font::Value,
                 R(R_ - 600.0f, T_, R_, T_ + 50.0f), c_.muted);

        if (mods.empty())
        {
            ui_.Text("Nothing installed yet. Search or browse CurseForge from the Mods screen.", Font::Lead, R(L_, T_ + 110.0f, R_, T_ + 160.0f),
                     c_.muted);
            Footer("Press B to go back");
            return;
        }

        const float rowH = 88.0f;
        const float gap = 10.0f;
        const float top = T_ + 100.0f;
        const int visible = static_cast<int>((B_ - 70.0f - top + gap) / (rowH + gap));
        const int count = static_cast<int>(mods.size());
        // the focused row sits mid list once the list is long enough to scroll
        int first = (std::max)(0, view_.installedIndex - visible / 2);
        first = (std::min)(first, (std::max)(0, count - visible));

        for (int at = first; at < count && at < first + visible; at++)
        {
            const InstalledMod &mod = mods[static_cast<std::size_t>(at)];
            const float y = top + (at - first) * (rowH + gap);
            const Rect row = R(L_, y, R_, y + rowH);
            const bool focused = at == view_.installedIndex;
            if (focused)
            {
                ui_.FillRound(row, 12.0f, c_.raised, 0.96f);
                FocusRing(row, 12.0f);
            }
            else
            {
                ui_.FillRound(row, 12.0f, c_.surface, 0.94f);
            }

            ui_.Text(mod.name, Font::RowStrong, R(row.left + 26.0f, row.top + 10.0f, row.right - 360.0f, row.top + 48.0f), c_.text);
            ui_.Text(mod.folder, Font::SmallMid, R(row.left + 26.0f, row.top + 46.0f, row.right - 360.0f, row.bottom - 10.0f), c_.muted);

            float right = row.right - 26.0f;
            const std::string version = mod.version.empty() ? std::string() : "v" + mod.version;
            ui_.Text(version, Font::Value, R(right - 200.0f, row.top, right, row.bottom), c_.muted);
            if (LoadBearing(mod))
            {
                right -= ui_.TextWidth(version, Font::Value) + 24.0f;
                const Rect pill = R(right - 104.0f, row.top + 29.0f, right, row.bottom - 29.0f);
                ui_.FillRound(pill, 15.0f, c_.accent, 0.16f);
                ui_.Text("NEEDED", Font::Overline, R(pill.left + 18.0f, pill.top, pill.right, pill.bottom), c_.accent);
            }
        }

        if (count > visible)
        {
            ui_.Text(std::to_string(view_.installedIndex + 1) + " of " + std::to_string(count), Font::Value,
                     R(R_ - 300.0f, B_ - 38.0f, R_, B_), c_.muted);
        }
        Footer(view_.notice.empty() ? std::string("Press A to delete a mod, B to go back") : view_.notice, view_.notice.empty() ? 0 : c_.accent);
    }

    void Confirm()
    {
        const std::vector<InstalledMod> &mods = InstalledList();
        if (view_.installedIndex < 0 || view_.installedIndex >= static_cast<int>(mods.size()))
        {
            return;
        }
        const InstalledMod &mod = mods[static_cast<std::size_t>(view_.installedIndex)];
        const bool needed = LoadBearing(mod);

        Header("Installed mods");
        ui_.Fill(R(0.0f, 0.0f, ui_.Width(), ui_.Height()), 0x000000, 0.55f);

        const float w = 920.0f;
        const float h = needed ? 470.0f : 410.0f;
        const float left = std::floor((ui_.Width() - w) * 0.5f);
        const float top = std::floor((ui_.Height() - h) * 0.5f);
        const Rect dialog = R(left, top, left + w, top + h);
        ui_.FillRound(dialog, 20.0f, c_.surface);

        const float x = dialog.left + 48.0f;
        float y = dialog.top + 44.0f;
        ui_.Circle(x + 32.0f, y + 32.0f, 32.0f, c_.danger, 0.16f);
        ui_.Icon(kIconDelete, R(x, y, x + 64.0f, y + 64.0f), c_.danger, true);
        y += 92.0f;
        ui_.Text("Delete " + mod.name + "?", Font::Title, R(x, y, dialog.right - 48.0f, y + 56.0f), c_.text);
        y += 64.0f;
        ui_.Text("This removes the folder " + mod.folder + " and everything in it.", Font::Lead, R(x, y, dialog.right - 48.0f, y + 70.0f), c_.muted);
        y += 50.0f;
        if (needed)
        {
            ui_.Icon(kIconWarning, R(x - 4.0f, y, x + 28.0f, y + 40.0f), c_.danger);
            ui_.Text("The game needs this one to run on the console.", Font::Lead, R(x + 36.0f, y, dialog.right - 48.0f, y + 40.0f), c_.danger);
            y += 60.0f;
        }

        const float buttonsY = dialog.bottom - 44.0f - 64.0f;
        Button(R(x, buttonsY, x + 260.0f, buttonsY + 64.0f), kIconDelete, "A  Delete it", false, Kind::Danger);
        Button(R(x + 280.0f, buttonsY, x + 540.0f, buttonsY + 64.0f), nullptr, "B  Keep it", false, Kind::Neutral);
    }

    void Pairing()
    {
        Dimmed(0.86f);
        Brand("STARDEW VALLEY UWP", c_.muted);
        const float column = L_ + (R_ - L_) * 0.52f;

        ui_.Text("Enter your key", Font::Display, R(L_ - 3.0f, T_ + 96.0f, column, T_ + 180.0f), c_.text);
        if (view_.pairingUrl.empty())
        {
            ui_.Text("Could not open the page on this console", Font::Lead, R(L_, T_ + 226.0f, column - 40.0f, T_ + 262.0f), c_.danger);
            ui_.Text(view_.pairingError, Font::Small, R(L_, T_ + 276.0f, column - 40.0f, T_ + 340.0f), c_.muted);
            ui_.Text("Upload curseforge.json to LocalState with the Device Portal instead.", Font::Small, R(L_, T_ + 350.0f, column - 40.0f, T_ + 420.0f),
                     c_.muted);
            Footer("Press B to go back");
            return;
        }

        ui_.Text("Open this on a phone on the same network", Font::Lead, R(L_, T_ + 226.0f, column - 40.0f, T_ + 262.0f), c_.muted);
        ui_.Text(view_.pairingUrl, Font::Title, R(L_, T_ + 262.0f, column - 40.0f, T_ + 320.0f), c_.accent);
        ui_.Text("Paste your CurseForge key into the page and send it. This screen closes by itself once the key arrives.", Font::Lead,
                 R(L_, T_ + 356.0f, column - 40.0f, T_ + 440.0f), c_.text);
        ui_.Text("The page only answers on this address, and it stops when you leave this screen.", Font::Small,
                 R(L_, T_ + 470.0f, column - 40.0f, T_ + 540.0f), c_.muted);

        QrCard(view_.pairingUrl, column);
        Footer("Press B to go back");
    }

    Ui &ui_;
    const LauncherView &view_;
    const Palette &c_;
    float L_ = 0.0f;
    float T_ = 0.0f;
    float R_ = 0.0f;
    float B_ = 0.0f;
};
}

void DrawLauncher(Ui &ui, const LauncherView &view)
{
    Painter(ui, view).Draw();
}
}
