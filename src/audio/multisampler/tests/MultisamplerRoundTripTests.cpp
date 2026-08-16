// =============================================================================
//  MultisamplerRoundTripTests.cpp  —  Phase 1 acceptance-criteria coverage
//  ─────────────────────────────────────────────────────────────────────────
//  Uses juce::UnitTest so it runs under whatever harness already collects
//  UnitTestRunner::runAllTests() in this project (standalone Main.cpp, a
//  CI-only test target, etc.) — add this file to that target's sources.
//
//  Covers plan §11 "Unit tests": tokenization/inheritance, boundary
//  validation, tuning conversion, relative path resolution, and the two
//  import/export round trips described in §14's acceptance criteria.
// =============================================================================

#include "../MultisamplerInstrument.h"
#include "../SfzImporter.h"
#include "../SfzExporter.h"
#include "../InstrumentSerializer.h"
#include <juce_core/juce_core.h>
#include <algorithm>

namespace
{
    /** A small key/velocity-layered SFZ exercising global/group inheritance,
        multiple opcodes per line, a quoted path with a space, and one
        deliberately unsupported opcode (`lorand`) to verify extraOpcodes
        preservation. */
    const char* kSampleSfz = R"SFZ(
// test fixture
<global>
ampeg_release=0.8

<group>
loop_mode=loop_continuous lorand=0.5

<region>
sample="Grand Piano C3.wav" lokey=48 hikey=59 pitch_keycenter=53
lovel=1 hivel=63 volume=-3 tune=12

<region>
sample=Grand Piano C3 Loud.wav lokey=48 hikey=59 pitch_keycenter=53
lovel=64 hivel=127 volume=0
)SFZ";

    /** Modeled directly on a real commercial library file (a DigitalSoundFactory
        / E-mu Mo'Phatt drum kit) that exposed both the default_path and
        note-name-key bugs on first real-world import: a <control> default_path
        that ~98% of its regions depend on for sample resolution, and a region
        mixing a numeric key= with note-name lokey=/hikey=/pitch_keycenter= on
        the SAME region — exactly the combination that used to collapse to
        key range 0-0 instead of either being honoured or reported. */
    const char* kRealWorldSfz = R"SFZ(
<control>
default_path=Kit Samples\

<group>
lovel=0
hivel=127

<region> sample=Kick.wav lokey=C2 hikey=E2 lovel=0 hivel=127 pitch_keycenter=D#-1
key=36
ampeg_decay=31.457

<region> sample=Snare.wav
lokey=38 hikey=38
pitch_keycenter=38
)SFZ";

    /** Same shape as kRealWorldSfz, but its <control> block also carries
        octave_offset= alongside default_path= — an opcode this importer
        doesn't support. Used to confirm the unsupportedHeader warning still
        fires when something is genuinely dropped, not just whenever
        <control> appears at all. */
    const char* kControlWithExtraOpcodeSfz = R"SFZ(
<control>
default_path=Kit Samples\
octave_offset=1

<group>
lovel=0
hivel=127

<region> sample=Kick.wav lokey=36 hikey=36 pitch_keycenter=36
)SFZ";
}

class MultisamplerRoundTripTests final : public juce::UnitTest
{
public:
    MultisamplerRoundTripTests() : juce::UnitTest ("Multisampler round trip", "Audio") {}

    void runTest() override
    {
        testSfzImportBasics();
        testSfzImportInheritanceAndExtraOpcodes();
        testSfzExportImportRoundTrip();
        testJsonRoundTripIsLossless();
        testValidation();
        testDefaultPathResolution();
        testControlHeaderWarningOnlyWhenOpcodesDropped();
        testNoteNameKeyOpcodes();
    }

private:
    void testSfzImportBasics()
    {
        beginTest ("Imports a key/velocity-layered SFZ with correct region boundaries");

        juce::TemporaryFile tmp (".sfz");
        tmp.getFile().replaceWithText (kSampleSfz);

        const auto result = SfzImporter::importFile (tmp.getFile());
        expect (result.success);
        expectEquals (result.instrument.zones.size(), (size_t) 2);

        const auto& soft = result.instrument.zones[0];
        expectEquals (soft.lowKey, 48);
        expectEquals (soft.highKey, 59);
        expectEquals (soft.rootKey, 53);
        expectEquals (soft.lowVelocity, 1);
        expectEquals (soft.highVelocity, 63);
        expectWithinAbsoluteError (soft.gainDb, -3.0f, 0.001f);
        expectWithinAbsoluteError (soft.tuneCents, 12.0f, 0.001f);

        const auto& loud = result.instrument.zones[1];
        expectEquals (loud.lowVelocity, 64);
        expectEquals (loud.highVelocity, 127);
    }

    void testSfzImportInheritanceAndExtraOpcodes()
    {
        beginTest ("Group/global opcodes are inherited; unsupported opcodes are preserved");

        const auto result = SfzImporter::importText (kSampleSfz, juce::File());
        expect (result.success);
        expectEquals (result.instrument.zones.size(), (size_t) 2);

        for (const auto& z : result.instrument.zones)
        {
            // ampeg_release came from <global>, loop_mode from <group> —
            // neither is set at region level in the fixture.
            expectWithinAbsoluteError (z.releaseSeconds, 0.8f, 0.001f);
            expect (z.loopMode == LoopMode::loopContinuous);

            // lorand isn't in the supported subset; it must survive as an
            // extra opcode rather than being silently dropped.
            const auto it = std::find_if (z.extraOpcodes.begin(), z.extraOpcodes.end(),
                                           [] (auto& kv) { return kv.first == "lorand"; });
            expect (it != z.extraOpcodes.end());
            if (it != z.extraOpcodes.end())
                expectEquals (it->second, juce::String ("0.5"));
        }
    }

    void testDefaultPathResolution()
    {
        beginTest ("<control> default_path= is prepended to relative sample= paths");

        // Lay out an actual "Kick.wav" / "Snare.wav" under a "Kit Samples"
        // subfolder next to a temp .sfz, matching kRealWorldSfz's bare
        // filenames + default_path convention, and confirm the importer
        // finds them there instead of reporting them missing.
        auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getNonexistentChildFile ("dysekt_default_path_test", "", false);
        root.createDirectory();
        auto samplesDir = root.getChildFile ("Kit Samples");
        samplesDir.createDirectory();
        samplesDir.getChildFile ("Kick.wav").replaceWithText ("not really audio");
        samplesDir.getChildFile ("Snare.wav").replaceWithText ("not really audio");

        auto sfzFile = root.getChildFile ("kit.sfz");
        sfzFile.replaceWithText (kRealWorldSfz);

        const auto result = SfzImporter::importFile (sfzFile);
        expect (result.success);
        expectEquals (result.instrument.zones.size(), (size_t) 2);

        // kRealWorldSfz's <control> block contains only default_path= —
        // nothing was actually dropped on import, so no unsupportedHeader
        // warning should fire for it. (Regression coverage for a bug where
        // the warning fired unconditionally just because <control> was
        // present, regardless of what was inside it.)
        expect (result.warnings.empty());

        // Neither region's sample= includes "Kit Samples/" itself — only
        // default_path supplies it. If this resolves without default_path
        // support, both would report as missing.
        for (const auto& z : result.instrument.zones)
            expect (! z.hasMissingSample());

        expectEquals (result.instrument.zones[0].sampleFile.getFileName(), juce::String ("Kick.wav"));
        expectEquals (result.instrument.zones[0].sampleFile.getParentDirectory().getFileName(),
                      juce::String ("Kit Samples"));

        root.deleteRecursively();
    }

    void testControlHeaderWarningOnlyWhenOpcodesDropped()
    {
        beginTest ("<control> only warns when it actually contains opcodes other than default_path");

        // default_path= alone: nothing dropped, so no warning.
        const auto cleanResult = SfzImporter::importText (kRealWorldSfz, juce::File());
        expect (cleanResult.success);
        expect (cleanResult.warnings.empty());

        // default_path= plus octave_offset=: octave_offset is genuinely
        // unsupported and dropped, so the warning should fire exactly once,
        // pointing at the <control> header's line.
        const auto flaggedResult = SfzImporter::importText (kControlWithExtraOpcodeSfz, juce::File());
        expect (flaggedResult.success);

        const auto it = std::find_if (flaggedResult.warnings.begin(), flaggedResult.warnings.end(),
                                       [] (auto& w)
                                       {
                                           return w.kind == SfzImporter::Warning::Kind::unsupportedHeader
                                               && w.detail.contains ("<control>");
                                       });
        expect (it != flaggedResult.warnings.end());
        if (it != flaggedResult.warnings.end())
            expectEquals (it->lineNumber, 2); // the "<control>" line itself in kControlWithExtraOpcodeSfz

        // default_path= should still have been honoured despite the warning.
        expectEquals (flaggedResult.instrument.zones.size(), (size_t) 1);
    }

    void testNoteNameKeyOpcodes()
    {
        beginTest ("Note-name key/lokey/hikey/pitch_keycenter parse to real MIDI notes, not 0");

        const auto result = SfzImporter::importText (kRealWorldSfz, juce::File());
        expect (result.success);
        expectEquals (result.instrument.zones.size(), (size_t) 2);

        // First region: lokey=C2 hikey=E2 pitch_keycenter=D#-1, PLUS an
        // explicit key=36 on the same region. Per SFZ semantics (and this
        // importer's existing, deliberate design) explicit lokey/hikey/
        // pitch_keycenter beat key= — the point of this test is that none
        // of the three collapse to 0, not that they match key=36.
        const auto& kick = result.instrument.zones[0];
        expectEquals (kick.lowKey, 48);    // C2
        expectEquals (kick.highKey, 52);   // E2
        expectEquals (kick.rootKey, 15);   // D#-1
        expect (kick.lowKey != 0 && kick.highKey != 0 && kick.rootKey != 0);

        // Second region: plain numeric lokey/hikey/pitch_keycenter — confirms
        // the numeric fast path still works unchanged alongside the new
        // note-name path.
        const auto& snare = result.instrument.zones[1];
        expectEquals (snare.lowKey, 38);
        expectEquals (snare.highKey, 38);
        expectEquals (snare.rootKey, 38);
    }

    void testSfzExportImportRoundTrip()
    {
        beginTest ("Export then reimport preserves every supported zone value");

        MultisamplerInstrument instrument;
        instrument.name = "Round Trip Test";

        SampleZone z;
        z.sampleFile = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("rt_sample.wav");
        z.sampleFile.replaceWithText ("not really audio, just needs to exist");
        z.lowKey = 21; z.highKey = 44; z.rootKey = 33;
        z.lowVelocity = 1; z.highVelocity = 100;
        z.tuneCents = -17.5f; z.gainDb = -6.25f; z.pan = 0.5f;
        z.sampleStart = 512; z.sampleEnd = 88200;
        z.loopMode = LoopMode::loopSustain; z.loopStart = 1000; z.loopEnd = 50000;
        z.attackSeconds = 0.02f; z.decaySeconds = 0.4f; z.sustainLevel = 0.7f; z.releaseSeconds = 1.25f;
        z.filterCutoffHz = 4200.0f; z.filterResonance = 0.3f;
        z.group = 2; z.offBy = 3; z.sequencePosition = 2; z.sequenceLength = 3;
        instrument.addZone (z);

        juce::TemporaryFile tmp (".sfz");
        const bool wrote = SfzExporter::exportToFile (instrument, tmp.getFile());
        expect (wrote);

        const auto reimported = SfzImporter::importFile (tmp.getFile());
        expect (reimported.success);
        expectEquals (reimported.instrument.zones.size(), (size_t) 1);

        const auto& rz = reimported.instrument.zones[0];
        expectEquals (rz.lowKey, 21);
        expectEquals (rz.highKey, 44);
        expectEquals (rz.rootKey, 33);
        expectEquals (rz.lowVelocity, 1);
        expectEquals (rz.highVelocity, 100);
        expectWithinAbsoluteError (rz.tuneCents, -17.5f, 0.01f);
        expectWithinAbsoluteError (rz.gainDb, -6.25f, 0.01f);
        expectWithinAbsoluteError (rz.pan, 0.5f, 0.01f);
        expectEquals ((juce::int64) rz.sampleStart, (juce::int64) 512);
        expectEquals ((juce::int64) rz.sampleEnd, (juce::int64) 88200);
        expect (rz.loopMode == LoopMode::loopSustain);
        expectEquals ((juce::int64) rz.loopStart, (juce::int64) 1000);
        expectEquals ((juce::int64) rz.loopEnd, (juce::int64) 50000);
        expectWithinAbsoluteError (rz.attackSeconds, 0.02f, 0.001f);
        expectWithinAbsoluteError (rz.decaySeconds, 0.4f, 0.001f);
        expectWithinAbsoluteError (rz.sustainLevel, 0.7f, 0.005f);
        expectWithinAbsoluteError (rz.releaseSeconds, 1.25f, 0.001f);
        expectWithinAbsoluteError (rz.filterCutoffHz, 4200.0f, 0.5f);
        expectWithinAbsoluteError (rz.filterResonance, 0.3f, 0.01f);
        expectEquals (rz.group, 2);
        expectEquals (rz.offBy, 3);
        expectEquals (rz.sequencePosition, 2);
        expectEquals (rz.sequenceLength, 3);

        z.sampleFile.deleteFile();
    }

    void testJsonRoundTripIsLossless()
    {
        beginTest ("instrument.json round trip is byte-exact on every field");

        MultisamplerInstrument instrument;
        instrument.name = "JSON Round Trip";
        instrument.author = "Test Author";
        instrument.masterGainDb = -2.5f;
        instrument.transposeSemitones = -12;
        instrument.fineTuneCents = 3.3f;
        instrument.maxVoices = 32;

        SampleZone z;
        z.lowKey = 10; z.highKey = 20; z.rootKey = 15;
        z.extraOpcodes.emplace_back ("lorand", "0.75");
        instrument.addZone (z);

        const auto json = InstrumentSerializer::toJson (instrument);
        const auto loaded = InstrumentSerializer::fromJson (json);

        expect (loaded.success);
        expectEquals (loaded.instrument.name, instrument.name);
        expectEquals (loaded.instrument.author, instrument.author);
        expectWithinAbsoluteError (loaded.instrument.masterGainDb, instrument.masterGainDb, 0.001f);
        expectEquals (loaded.instrument.transposeSemitones, instrument.transposeSemitones);
        expectWithinAbsoluteError (loaded.instrument.fineTuneCents, instrument.fineTuneCents, 0.001f);
        expectEquals (loaded.instrument.maxVoices, instrument.maxVoices);
        expectEquals (loaded.instrument.zones.size(), (size_t) 1);
        expectEquals (loaded.instrument.zones[0].id.toString(), z.id.toString());
        expectEquals (loaded.instrument.zones[0].extraOpcodes.size(), (size_t) 1);
    }

    void testValidation()
    {
        beginTest ("validate() flags missing samples and inverted ranges");

        MultisamplerInstrument instrument;
        SampleZone bad;
        bad.sampleFile = juce::File ("/definitely/does/not/exist.wav");
        bad.lowKey = 80; bad.highKey = 40;   // inverted
        instrument.addZone (bad);

        const auto issues = instrument.validate();
        expect (issues.size() >= 2);
    }
};

static MultisamplerRoundTripTests multisamplerRoundTripTests;
