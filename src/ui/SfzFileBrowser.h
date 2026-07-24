#pragma once
// =============================================================================
//  SfzFileBrowser.h  —  Self-contained inline directory browser component
// =============================================================================
//  Shared by SfzDropdownPanel and SfzModulePanel.
//  Mode::kSfz      — lists *.sf2 / *.sfz, onFileChosen fires on selection
//  Mode::kAddZone  — lists audio files (wav/aif/flac/ogg), onFileChosen fires
// =============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

class SfzFileBrowser : public juce::Component,
                       public juce::ListBoxModel,
                       private juce::Timer
{
public:
    /** Controls which file types are listed and what happens on selection. */
    enum class Mode { kSfz, kSf2, kAddZone };

    /** Called when the user double-clicks a file (type depends on current mode). */
    std::function<void (const juce::File&)> onFileSingleClicked;
    std::function<void (const juce::File&)> onFileChosen;
    /** Called after a new SF2/SFZ file has been accepted (any path). */
    std::function<void (const juce::File&)> onFileLoaded;
    /** Called when the user explicitly closes the browser (Esc / icon click). */
    std::function<void()> onDismiss;

    SfzFileBrowser();
    void setMode (Mode m);               ///< Switch between SFZ-pick and sample-pick modes
    Mode getMode () const { return mode; }
    ~SfzFileBrowser() override;

    /**
     * Returns true if a file (identified by its name/extension — a full path,
     * bare filename, or archive entry name all work) belongs in the given
     * mode's listing:
     *   kAddZone -> wav/aif/aiff/flac/ogg/mp3
     *   kSf2     -> sf2
     *   kSfz     -> sfz
     *
     * This is the single source of truth for "does this extension belong in
     * this mode", shared with anything that lists files alongside the local
     * filesystem browser (e.g. ArchiveIntegration results in FileBrowserPanel)
     * so those listings can't drift out of sync with what the local tree
     * shows for the same mode.
     */
    static bool matchesMode (Mode m, const juce::String& fileNameOrPath);

    // juce::Component overrides
    void paint   (juce::Graphics&) override;
    void resized () override;
    void mouseDown  (const juce::MouseEvent&) override;
    void mouseMove  (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override { return false; }

    // Open to a specific root directory (call before making visible)
    void setRootDirectory (const juce::File& dir);
    void showDrives   ();
    bool hasNavigated () const { return navigated; }
    juce::File getCurrentDirectory() const { return currentDir; }

    // ── ListBoxModel ──────────────────────────────────────────────────────────
    int  getNumRows()                                                  override;
    void paintListBoxItem (int row, juce::Graphics& g,
                           int w, int h, bool selected)               override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&)  override;
    void listBoxItemClicked       (int row, const juce::MouseEvent&)  override;
    juce::String getTooltipForRow (int row)                           override;

private:
    void   navigateTo      (const juce::File& dir);
    void   navigateUp      ();
    void   navigateToRoots ();
    void   loadRow         (int row);
    juce::File fileForRow  (int row) const;
    bool   isDirectory     (int row) const;

    void timerCallback() override;
    void rebuildList();
    void rebuildFsList();    // populates rows from currentDir (real filesystem)
    void rebuildZipList();   // populates rows from inside activeZipFile at zipCurrentPath

    // ── Zip-archive drill-in ──────────────────────────────────────────────────
    // Lets the user double-click a .zip the same way they'd double-click a
    // folder, browse its contents (one archive deep — nested zip-in-zip is not
    // supported, per sf2-zip-loading-plan.md's open question), and pick a
    // matching .sf2/.sfz entry from inside it.
    //
    // STOPGAP (plan Step 6, build-order item 1): picking an entry extracts it
    // to a real temp file on disk and hands that off through the existing
    // juce::File-based onFileChosen callback, unchanged — no changes anywhere
    // downstream of this class (Command::fileParam, loadSoundFontAsync(), or
    // any onLoadRequest/onFileChosen handler). This does NOT avoid the on-disk
    // duplicate copy the full feature is meant to eliminate; that requires the
    // FluidSynth memory-loader work in Step 3/4, not implemented here.
    void enterZip      (const juce::File& zipFile);
    void exitZip       ();
    void navigateZipTo (const juce::String& innerPath);   // "" = zip root, else "Sub/Folder/"
    bool extractZipEntryToTemp (const juce::File& zipFile, const juce::String& entryPath,
                                 juce::File& outTempFile);

    // .sfz text files reference sample audio via paths relative to the .sfz
    // file's own folder (and may #include sibling .sfz fragments). Extracting
    // only the clicked entry (as extractZipEntryToTemp does) leaves those
    // relative references dangling, so sfizz_load_file() finds no samples.
    // This extracts every entry that shares the .sfz's containing folder in
    // the archive, preserving relative layout, and returns the path to the
    // re-homed .sfz inside that folder.
    bool extractSfzFolderToTemp (const juce::File& zipFile, const juce::String& entryPath,
                                  juce::File& outTempFile);

    struct BrowserRow
    {
        enum class Kind { Directory, File, ZipFolder, ZipFile };
        Kind         kind = Kind::File;
        juce::File   file;          // real fs entry (Directory/File); owning .zip file (ZipFolder/ZipFile)
        juce::String zipPath;       // full entry path inside the archive (ZipFolder/ZipFile only)
        juce::String displayName;   // row label
    };

    Mode       mode       { Mode::kSfz };
    juce::File currentDir;

    juce::File   activeZipFile;    // non-empty while browsing inside a zip
    juce::String zipCurrentPath;   // "" = zip root, else "Sub/Folder/" prefix

    juce::Array<BrowserRow> rows;
    juce::ListBox list;

    juce::Rectangle<int> breadcrumbZone;
    juce::Rectangle<int> upBtnZone;
    bool                 atVirtualRoot { false };
    bool                 navigated     { false };

    static constexpr int kBreadcrumbH = 22;
    static constexpr int kRowH        = 26;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SfzFileBrowser)
};
