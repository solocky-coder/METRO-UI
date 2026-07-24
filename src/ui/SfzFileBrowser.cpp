// =============================================================================
//  SfzFileBrowser.cpp  —  Implementation of the self-contained directory browser
// =============================================================================
#include "SfzFileBrowser.h"
#include "DysektLookAndFeel.h"
#include <algorithm>

// ── Themed folder glyph ───────────────────────────────────────────────────────
// Colour-emoji glyphs (like U+1F4C1) render from their own embedded colour
// palette in most font backends, so Graphics::setColour() has no effect on
// them — that's why the folder icon was stuck yellow regardless of theme.
// This draws a simple vector folder outline that actually respects theme.accent.
static void drawFolderGlyph (juce::Graphics& g, juce::Rectangle<float> box, juce::Colour colour)
{
    const float tabW  = box.getWidth()  * 0.45f;
    const float tabH  = box.getHeight() * 0.30f;
    const float bodyY = box.getY() + tabH;

    juce::Path p;
    p.startNewSubPath (box.getX(),                      box.getBottom());
    p.lineTo           (box.getX(),                      bodyY);
    p.lineTo           (box.getX() + tabW,               bodyY);
    p.lineTo           (box.getX() + tabW + tabH * 0.8f, box.getY());
    p.lineTo           (box.getRight(),                  box.getY());
    p.lineTo           (box.getRight(),                  box.getBottom());
    p.closeSubPath();

    g.setColour (colour);
    g.strokePath (p, juce::PathStrokeType (1.3f));
}

// =============================================================================
//  SfzFileBrowser
// =============================================================================

SfzFileBrowser::SfzFileBrowser()
{
    list.setModel (this);
    list.setRowHeight (kRowH);
    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    list.setColour (juce::ListBox::outlineColourId,    juce::Colours::transparentBlack);
    addAndMakeVisible (list);

    // Don't call navigateTo() in the constructor — layout isn't ready yet.
    // But DO initialise currentDir to a valid root so that navigateUp() never
    // sees a default-constructed File() and accidentally fires navigateToRoots().
    {
        juce::Array<juce::File> roots;
        juce::File::findFileSystemRoots (roots);
        if (roots.size() > 0)
            currentDir = roots[0];
    }
}

SfzFileBrowser::~SfzFileBrowser()
{
    stopTimer();
    list.setModel (nullptr);
}

// ── paint ────────────────────────────────────────────────────────────────────

void SfzFileBrowser::paint (juce::Graphics& g)
{
    const auto& theme = getTheme();

    // Self-computed scale factor — same pattern as SliceLcdDisplay/DualLcdControlFrame.
    // Derived from the breadcrumb bar's actual height vs. its design height, so it
    // tracks whatever resized() decided (which itself tracks overall UI scale).
    const float sf = breadcrumbZone.getHeight() > 0
                        ? (float) breadcrumbZone.getHeight() / (float) kBreadcrumbH
                        : 1.0f;

    // Background
    g.setColour (theme.darkBar.darker (0.45f));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 0.0f);

    // Breadcrumb bar background
    g.setColour (theme.darkBar.darker (0.20f));
    g.fillRect (breadcrumbZone);

    // Back/up button (← — navigate to parent directory)
    {
        const bool canGoUp  = !atVirtualRoot;
        const bool upHover  = upBtnZone.contains (getMouseXYRelative()) && canGoUp;
        if (upHover)
        {
            g.setColour (theme.accent.withAlpha (0.18f));
            g.fillRoundedRectangle (upBtnZone.toFloat(), 0.0f);
        }
        g.setFont (DysektLookAndFeel::makeFont (14.0f * sf));
        g.setColour (canGoUp ? theme.accent.withAlpha (0.90f)
                             : theme.accent.withAlpha (0.30f));
        g.drawText (u8"\u2190", upBtnZone, juce::Justification::centred, false);
    }

    // Current path text — shows "Drives" label when in the virtual root view
    {
        const auto pathArea = breadcrumbZone.withTrimmedLeft (upBtnZone.getWidth() + 4)
                                            .withTrimmedRight (4);
        g.setFont (DysektLookAndFeel::makeFont (12.0f * sf));
        g.setColour (theme.foreground.withAlpha (0.55f));

        // Show last 2 path segments so it fits; show "Drives" in virtual-root mode;
        // show "archive.zip › inner / path" while browsing inside a zip.
        juce::String display;
        if (atVirtualRoot)
        {
            display = "Drives";
        }
        else if (activeZipFile.existsAsFile())
        {
            display = activeZipFile.getFileName();
            if (zipCurrentPath.isNotEmpty())
            {
                const auto segs = juce::StringArray::fromTokens (
                    zipCurrentPath.dropLastCharacters (1), "/", "");
                for (auto& seg : segs)
                    display << "  \u203a  " << seg;
            }
        }
        else
        {
            const auto parts = juce::StringArray::fromTokens (
                currentDir.getFullPathName(), juce::File::getSeparatorString(), "");
            const int n = parts.size();
            if      (n == 0) display = "/";
            else if (n <= 2) display = currentDir.getFullPathName();
            else             display = u8"\u2026" + juce::File::getSeparatorString()
                                     + parts[n - 2] + juce::File::getSeparatorString()
                                     + parts[n - 1];
        }

        g.drawText (display, pathArea, juce::Justification::centredLeft, true);
    }

    // Separator between breadcrumb and list
    g.setColour (theme.accent.withAlpha (0.12f));
    g.fillRect (0, breadcrumbZone.getBottom(), getWidth(), 1);
}

// ── resized ───────────────────────────────────────────────────────────────────

void SfzFileBrowser::resized()
{
    // Self-computed scale factor — same pattern as SliceLcdDisplay/DualLcdControlFrame.
    // This browser fills the width of its parent module panel, which itself is laid
    // out proportional to overall UI/window scale, so getWidth() tracks that scale.
    const float sf = juce::jlimit (0.75f, 2.5f, (float) getWidth() / 1114.0f);

    const int breadcrumbH = juce::roundToInt (kBreadcrumbH * sf);
    const int rowH        = juce::roundToInt (kRowH        * sf);
    const int upW         = juce::roundToInt (24.0f        * sf);

    breadcrumbZone = { 0, 0, getWidth(), breadcrumbH };
    upBtnZone      = { 0, 1, upW, breadcrumbH - 2 };

    list.setRowHeight (rowH);
    list.setBounds (0, breadcrumbH + 1, getWidth(), getHeight() - breadcrumbH - 1);
}

// ── mouseDown ────────────────────────────────────────────────────────────────

void SfzFileBrowser::mouseMove (const juce::MouseEvent&)
{
    repaint (breadcrumbZone);  // refresh hover highlight on up/drive buttons
}

void SfzFileBrowser::mouseDown (const juce::MouseEvent& e)
{
    if (upBtnZone.contains (e.getPosition()))
    {
        navigateUp();
        return;
    }
    // Clicks below the breadcrumb are handled by the ListBox itself
}

// ── navigation ───────────────────────────────────────────────────────────────

void SfzFileBrowser::navigateTo (const juce::File& dir)
{
    if (! dir.isDirectory()) return;
    activeZipFile  = {};   // real-directory navigation always exits any zip view
    zipCurrentPath = {};
    atVirtualRoot = false;
    navigated     = true;
    currentDir = dir;
    rebuildList();
    repaint();
}

void SfzFileBrowser::navigateUp()
{
    if (! isVisible()) return;   // ignore if browser is closed

    if (activeZipFile.existsAsFile())
    {
        if (zipCurrentPath.isNotEmpty())
        {
            // Pop one path segment inside the archive: "A/B/" -> "A/"
            const auto trimmed   = zipCurrentPath.dropLastCharacters (1);   // drop trailing '/'
            const auto lastSlash = trimmed.lastIndexOfChar ('/');
            navigateZipTo (lastSlash >= 0 ? trimmed.substring (0, lastSlash + 1) : juce::String());
        }
        else
        {
            exitZip();   // already at the archive's own root — back out to the real folder
        }
        return;
    }

    if (atVirtualRoot) return;
    const auto parent = currentDir.getParentDirectory();
    if (parent == currentDir)
        navigateToRoots();   // already at a filesystem root — go to drive picker
    else
        navigateTo (parent);
}

void SfzFileBrowser::navigateToRoots()
{
    if (! isVisible()) return;   // ignore if browser is closed

    activeZipFile  = {};   // drive picker always exits any zip view
    zipCurrentPath = {};

    // Use JUCE's cross-platform API to enumerate all filesystem roots.
    // On Windows this yields every present drive letter (C:\, D:\, etc.).
    // On macOS/Linux it yields /.
    juce::Array<juce::File> roots;
    juce::File::findFileSystemRoots (roots);

#if JUCE_MAC
    // findFileSystemRoots returns only / on macOS; /Volumes/* are the actual
    // named mounts (external drives, network shares, other partitions).
    auto volumes = juce::File ("/Volumes");
    if (volumes.isDirectory())
    {
        auto vols = volumes.findChildFiles (juce::File::findDirectories, false);
        vols.sort();
        for (auto& v : vols)
        {
            bool dupe = false;
            for (auto& r : roots)
                if (r == v) { dupe = true; break; }
            if (! dupe)
                roots.add (v);
        }
    }
#elif !JUCE_WINDOWS
    // Linux: /media and /mnt are conventional mountpoints for removable drives.
    for (const char* mp : { "/media", "/mnt", "/run/media" })
    {
        juce::File m (mp);
        if (m.isDirectory())
        {
            bool dupe = false;
            for (auto& r : roots)
                if (r == m) { dupe = true; break; }
            if (! dupe)
                roots.add (m);
        }
    }
#endif

    rows.clear();
    for (auto& r : roots)
        rows.add ({ BrowserRow::Kind::Directory, r, {}, r.getFileName() });

    atVirtualRoot = true;   // breadcrumb shows "Drives" label instead of a path
    navigated     = true;
    list.updateContent();
    list.repaint();
    repaint();
}

bool SfzFileBrowser::matchesMode (Mode m, const juce::String& fileNameOrPath)
{
    if (m == Mode::kAddZone)
        return fileNameOrPath.endsWithIgnoreCase (".wav")  || fileNameOrPath.endsWithIgnoreCase (".aif")
            || fileNameOrPath.endsWithIgnoreCase (".aiff") || fileNameOrPath.endsWithIgnoreCase (".flac")
            || fileNameOrPath.endsWithIgnoreCase (".ogg")  || fileNameOrPath.endsWithIgnoreCase (".mp3");
    if (m == Mode::kSf2)
        return fileNameOrPath.endsWithIgnoreCase (".sf2");
    return fileNameOrPath.endsWithIgnoreCase (".sfz");
}

void SfzFileBrowser::setMode (Mode m)
{
    mode = m;
    rebuildList();
}

void SfzFileBrowser::rebuildList()
{
    rows.clear();

    if (activeZipFile.existsAsFile())
        rebuildZipList();
    else
        rebuildFsList();

    list.updateContent();
    list.repaint();
    repaint();   // breadcrumb path has changed
}

void SfzFileBrowser::rebuildFsList()
{
    // Directories first (hidden files excluded)
    auto dirs = currentDir.findChildFiles (
        juce::File::findDirectories, false, "*");
    dirs.removeIf ([] (const juce::File& f) { return f.isHidden(); });
    dirs.sort();

    // Matching files — pattern depends on current mode
    const auto* pattern = (mode == Mode::kAddZone)
                            ? "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3"
                            : (mode == Mode::kSf2)
                              ? "*.sf2"
                              : "*.sfz";   // SFZ-Player: SFZ files only

    auto files = currentDir.findChildFiles (
        juce::File::findFiles, false, pattern);
    files.removeIf ([] (const juce::File& f) { return f.isHidden(); });
    files.sort();

    // .zip archives are always listed too (independent of the mode's
    // extension filter) so the user can drill into one and pick a matching
    // .sf2/.sfz entry — see rebuildZipList(). Not offered in kAddZone mode
    // (zone-builder sample picking), since that's plain audio files, not
    // instrument archives.
    juce::Array<juce::File> zips;
    if (mode != Mode::kAddZone)
    {
        zips = currentDir.findChildFiles (juce::File::findFiles, false, "*.zip");
        zips.removeIf ([] (const juce::File& f) { return f.isHidden(); });
        zips.sort();
    }

    for (auto& d : dirs)
        rows.add ({ BrowserRow::Kind::Directory, d, {}, d.getFileName() });
    for (auto& z : zips)
        rows.add ({ BrowserRow::Kind::File, z, {}, z.getFileName() });
    for (auto& f : files)
        rows.add ({ BrowserRow::Kind::File, f, {}, f.getFileName() });
}

void SfzFileBrowser::rebuildZipList()
{
    juce::ZipFile zip (activeZipFile);

    juce::StringArray        seenFolders;
    juce::Array<BrowserRow>  folderRows, fileRows;

    for (int i = 0; i < zip.getNumEntries(); ++i)
    {
        const auto* entry = zip.getEntry (i);
        if (entry == nullptr) continue;

        // Zip entry paths always use '/'; only look at entries under the
        // directory level we're currently viewing inside the archive.
        const auto path = entry->filename.replaceCharacter ('\\', '/');
        if (! path.startsWith (zipCurrentPath)) continue;

        const auto rel = path.substring (zipCurrentPath.length());
        if (rel.isEmpty()) continue;   // the directory placeholder entry for this level itself

        const int slash = rel.indexOfChar ('/');
        if (slash >= 0)
        {
            // Immediate subfolder — collapse every entry under it into one row.
            const auto folderName = rel.substring (0, slash);
            if (folderName.isEmpty() || seenFolders.contains (folderName)) continue;
            seenFolders.add (folderName);
            folderRows.add ({ BrowserRow::Kind::ZipFolder, activeZipFile,
                               zipCurrentPath + folderName + "/", folderName });
        }
        else
        {
            // A file directly inside the current directory level.
            if (! matchesMode (mode, rel)) continue;
            fileRows.add ({ BrowserRow::Kind::ZipFile, activeZipFile, path, rel });
        }
    }

    const auto byName = [] (const BrowserRow& a, const BrowserRow& b)
    { return a.displayName.compareIgnoreCase (b.displayName) < 0; };
    std::sort (folderRows.begin(), folderRows.end(), byName);
    std::sort (fileRows.begin(),   fileRows.end(),   byName);

    for (auto& r : folderRows) rows.add (r);
    for (auto& r : fileRows)   rows.add (r);
}

void SfzFileBrowser::enterZip (const juce::File& zipFile)
{
    activeZipFile  = zipFile;
    zipCurrentPath = {};
    atVirtualRoot  = false;
    navigated      = true;
    rebuildList();
    repaint();
}

void SfzFileBrowser::exitZip()
{
    activeZipFile  = {};
    zipCurrentPath = {};
    rebuildList();   // rebuilds from currentDir — the real folder that held the zip
    repaint();
}

void SfzFileBrowser::navigateZipTo (const juce::String& innerPath)
{
    zipCurrentPath = innerPath;
    rebuildList();
    repaint();
}

bool SfzFileBrowser::extractZipEntryToTemp (const juce::File& zipFile, const juce::String& entryPath,
                                             juce::File& outTempFile)
{
    juce::ZipFile zip (zipFile);
    const int index = zip.getIndexOfFileName (entryPath);
    if (index < 0) return false;

    std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (index));
    if (in == nullptr) return false;

    // STOPGAP (see sf2-zip-loading-plan.md Step 6): extracts the full
    // decompressed entry to a real temp file and hands that off through the
    // existing juce::File-based load path unchanged. This still duplicates
    // disk space for the extracted copy — avoiding that is the whole point
    // of the eventual feature, via Step 3/4's in-memory FluidSynth loader,
    // not implemented here. This only proves out the browsing UI (Step 1).
    const auto entryName = juce::File (entryPath).getFileName();
    const auto base       = juce::File (entryName).getFileNameWithoutExtension();
    const auto ext        = juce::File (entryName).getFileExtension();   // includes leading '.'

    auto tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("DYSEKT-SF_zip_extract");
    tempDir.createDirectory();

    const auto tempFile = tempDir.getChildFile (
        base + "_" + juce::String (juce::Time::getCurrentTime().toMilliseconds()) + ext);

    std::unique_ptr<juce::FileOutputStream> out (tempFile.createOutputStream());
    if (out == nullptr) return false;

    const auto written = out->writeFromInputStream (*in, -1);
    out->flush();
    if (written <= 0) return false;

    outTempFile = tempFile;
    return true;
}

bool SfzFileBrowser::extractSfzFolderToTemp (const juce::File& zipFile, const juce::String& entryPath,
                                              juce::File& outTempFile)
{
    juce::ZipFile zip (zipFile);

    // Containing folder of the .sfz entry inside the archive, e.g.
    // "Kits/Grand Piano/" for "Kits/Grand Piano/piano.sfz", or "" if the
    // .sfz sits at the archive root. Every sample/#include reference in the
    // .sfz is resolved relative to this folder, so everything under it needs
    // to land on disk with the same relative layout for sfizz to find it.
    const int lastSlash  = entryPath.lastIndexOfChar ('/');
    const auto folderPfx = lastSlash >= 0 ? entryPath.substring (0, lastSlash + 1) : juce::String();

    auto tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("DYSEKT-SF_zip_extract")
                       .getChildFile ("sfz_" + juce::String (juce::Time::getCurrentTime().toMilliseconds()));
    if (! tempDir.createDirectory())
        return false;

    bool sfzWritten = false;
    juce::File sfzOut;

    for (int i = 0; i < zip.getNumEntries(); ++i)
    {
        const auto* entry = zip.getEntry (i);
        if (entry == nullptr) continue;

        const auto path = entry->filename.replaceCharacter ('\\', '/');
        if (! path.startsWith (folderPfx)) continue;      // outside this .sfz's folder
        if (path.endsWithChar ('/')) continue;             // directory entry, nothing to write

        const auto rel = path.substring (folderPfx.length());
        if (rel.isEmpty()) continue;

        const auto destFile = tempDir.getChildFile (rel);
        if (! destFile.getParentDirectory().createDirectory())
            continue;

        std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (i));
        if (in == nullptr) continue;

        std::unique_ptr<juce::FileOutputStream> out (destFile.createOutputStream());
        if (out == nullptr) continue;

        out->writeFromInputStream (*in, -1);
        out->flush();

        if (path == entryPath)
        {
            sfzOut     = destFile;
            sfzWritten = true;
        }
    }

    if (! sfzWritten) return false;

    outTempFile = sfzOut;
    return true;
}

void SfzFileBrowser::setRootDirectory (const juce::File& dir)
{
    navigateTo (dir);
}

void SfzFileBrowser::showDrives()
{
    navigateToRoots();
}

// ── ListBoxModel ──────────────────────────────────────────────────────────────

int SfzFileBrowser::getNumRows() { return rows.size(); }

bool SfzFileBrowser::isDirectory (int row) const
{
    if (row < 0 || row >= rows.size()) return false;
    const auto k = rows[row].kind;
    return k == BrowserRow::Kind::Directory || k == BrowserRow::Kind::ZipFolder;
}

juce::File SfzFileBrowser::fileForRow (int row) const
{
    if (row < 0 || row >= rows.size()) return {};
    return rows[row].file;   // real fs entry, or the owning .zip file for zip pseudo-rows
}

void SfzFileBrowser::paintListBoxItem (int row, juce::Graphics& g,
                                        int w, int h, bool selected)
{
    if (row < 0 || row >= rows.size()) return;

    const auto& theme = getTheme();
    const auto& r     = rows[row];
    const bool  isDir = (r.kind == BrowserRow::Kind::Directory || r.kind == BrowserRow::Kind::ZipFolder);

    // Self-computed scale factor — same pattern as SliceLcdDisplay/DualLcdControlFrame.
    // Derived straight from the actual row height JUCE gives us, so it always matches
    // whatever resized() set via list.setRowHeight(), with no shared state needed.
    const float sf = (float) h / (float) kRowH;

    if (selected)
    {
        g.setColour (theme.accent.withAlpha (0.14f));
        g.fillAll();
    }

    const int   iconColW = juce::roundToInt (22.0f * sf);
    const float textSize = 13.0f * sf;

    if (isDir)
    {
        // Vector folder glyph — replaces the color-emoji icon, which ignored
        // setColour() and was stuck yellow regardless of theme. Used for both
        // real directories and zip subfolder rows.
        auto iconBox = juce::Rectangle<float> (3.0f * sf, (float) h * 0.28f,
                                                (float) iconColW - 6.0f * sf, (float) h * 0.46f);
        drawFolderGlyph (g, iconBox, theme.accent.withAlpha (0.75f));

        g.setFont (DysektLookAndFeel::makeFont (textSize));
        g.setColour (selected ? theme.accent : theme.foreground.withAlpha (0.80f));
        g.drawText (r.displayName, iconColW + 6, 0, w - iconColW - 10, h,
                    juce::Justification::centredLeft, true);
    }
    else
    {
        // Extension badge (only for files with a known extension). Parsed off
        // displayName (pure string operation — safe for zip entries too, which
        // have no real on-disk path to query) so real files and zip entries
        // share the exact same painting logic.
        const juce::File nameParser (r.displayName);
        const auto ext = nameParser.getFileExtension().toUpperCase().trimCharactersAtStart (".");
        if (ext.isEmpty())
        {
            g.setFont (DysektLookAndFeel::makeFont (textSize));
            g.setColour (selected ? theme.accent : theme.foreground.withAlpha (0.80f));
            g.drawText (r.displayName, juce::roundToInt (6 * sf), 0,
                        w - juce::roundToInt (10 * sf), h,
                        juce::Justification::centredLeft, true);
        }
        else
        {
            const int  badgeW    = juce::roundToInt (36 * sf);
            const int  badgeH    = juce::roundToInt (14 * sf);
            const auto badgeRect = juce::Rectangle<int> (w - badgeW - juce::roundToInt (4 * sf),
                                                          (h - badgeH) / 2, badgeW, badgeH);
            g.setColour (theme.accent.withAlpha (0.18f));
            g.fillRoundedRectangle (badgeRect.toFloat(), 0.0f);
            g.setFont (DysektLookAndFeel::makeFont (10.0f * sf));
            g.setColour (theme.accent.withAlpha (0.80f));
            g.drawText (ext, badgeRect, juce::Justification::centred, false);

            // Filename
            g.setFont (DysektLookAndFeel::makeFont (textSize));
            g.setColour (selected ? theme.accent : theme.foreground.withAlpha (0.80f));
            g.drawText (nameParser.getFileNameWithoutExtension(), juce::roundToInt (6 * sf), 0,
                        w - badgeW - juce::roundToInt (14 * sf), h,
                        juce::Justification::centredLeft, true);
        }
    }
}

void SfzFileBrowser::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    // Single-click preview only applies to plain on-disk files. A zip entry
    // would need extracting to preview, which isn't worth doing on every
    // click/scroll-selection — preview for zip entries is skipped in this
    // stopgap; double-click still extracts and loads normally via loadRow().
    if (row >= 0 && row < rows.size())
    {
        const auto& r = rows[row];
        if (r.kind == BrowserRow::Kind::File && ! r.file.hasFileExtension ("zip") && onFileSingleClicked)
            onFileSingleClicked (r.file);
    }

    repaint();
}

void SfzFileBrowser::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    loadRow (row);
}

juce::String SfzFileBrowser::getTooltipForRow (int /*row*/)
{
    return {};
}

void SfzFileBrowser::loadRow (int row)
{
    if (row < 0 || row >= rows.size()) return;
    const auto& r = rows[row];

    switch (r.kind)
    {
        case BrowserRow::Kind::Directory:
            navigateTo (r.file);
            return;

        case BrowserRow::Kind::File:
            if (r.file.hasFileExtension ("zip"))
                enterZip (r.file);
            else if (onFileChosen)
                onFileChosen (r.file);
            return;

        case BrowserRow::Kind::ZipFolder:
            navigateZipTo (r.zipPath);
            return;

        case BrowserRow::Kind::ZipFile:
        {
            juce::File extracted;
            const bool isSfz = r.zipPath.endsWithIgnoreCase (".sfz");

            // .sfz files reference sample audio (and sometimes sibling .sfz
            // fragments via #include) by path relative to their own folder,
            // so the whole folder has to come out of the archive together —
            // a lone .sfz text file with no samples next to it won't load
            // anything. .sf2 is a single self-contained binary, so the plain
            // single-entry extraction is still correct there.
            const bool ok = isSfz
                ? extractSfzFolderToTemp (activeZipFile, r.zipPath, extracted)
                : extractZipEntryToTemp  (activeZipFile, r.zipPath, extracted);

            if (ok && onFileChosen)
                onFileChosen (extracted);
            return;
        }
    }
}

void SfzFileBrowser::timerCallback() {}
