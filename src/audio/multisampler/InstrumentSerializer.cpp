#include "InstrumentSerializer.h"

namespace
{
    juce::String samplePathForStorage (const SampleZone& z, const juce::File& bundleRoot)
    {
        if (z.sampleFile == juce::File())
            return {};
        if (bundleRoot != juce::File() && bundleRoot.isDirectory())
            return z.sampleFile.getRelativePathFrom (bundleRoot).replaceCharacter ('\\', '/');
        return z.sampleFile.getFullPathName();
    }

    juce::File resolveStoredPath (const juce::String& stored, const juce::File& bundleRoot)
    {
        if (stored.isEmpty())
            return {};
        const juce::File asIs (stored);
        if (juce::File::isAbsolutePath (stored))
            return asIs;
        if (bundleRoot != juce::File())
            return bundleRoot.getChildFile (stored);
        return asIs;
    }

    juce::var zoneToVar (const SampleZone& z, const juce::File& bundleRoot)
    {
        auto* o = new juce::DynamicObject();

        o->setProperty ("id", z.id.toString());
        o->setProperty ("sampleFile", samplePathForStorage (z, bundleRoot));

        o->setProperty ("lowKey", z.lowKey);
        o->setProperty ("highKey", z.highKey);
        o->setProperty ("rootKey", z.rootKey);
        o->setProperty ("lowVelocity", z.lowVelocity);
        o->setProperty ("highVelocity", z.highVelocity);

        o->setProperty ("tuneCents", z.tuneCents);
        o->setProperty ("gainDb", z.gainDb);
        o->setProperty ("pan", z.pan);

        o->setProperty ("sampleStart", (double) z.sampleStart);
        o->setProperty ("sampleEnd", (double) z.sampleEnd);
        o->setProperty ("loopStart", (double) z.loopStart);
        o->setProperty ("loopEnd", (double) z.loopEnd);
        o->setProperty ("loopMode", loopModeToOpcodeValue (z.loopMode));

        o->setProperty ("attackSeconds", z.attackSeconds);
        o->setProperty ("decaySeconds", z.decaySeconds);
        o->setProperty ("sustainLevel", z.sustainLevel);
        o->setProperty ("releaseSeconds", z.releaseSeconds);

        o->setProperty ("filterCutoffHz", z.filterCutoffHz);
        o->setProperty ("filterResonance", z.filterResonance);

        o->setProperty ("group", z.group);
        o->setProperty ("offBy", z.offBy);
        o->setProperty ("sequencePosition", z.sequencePosition);
        o->setProperty ("sequenceLength", z.sequenceLength);
        o->setProperty ("enabled", z.enabled);

        juce::Array<juce::var> extras;
        for (const auto& [k, v] : z.extraOpcodes)
        {
            auto* e = new juce::DynamicObject();
            e->setProperty ("key", k);
            e->setProperty ("value", v);
            extras.add (juce::var (e));
        }
        o->setProperty ("extraOpcodes", extras);

        return juce::var (o);
    }

    bool zoneFromVar (const juce::var& v, const juce::File& bundleRoot, SampleZone& outZone, juce::String& err)
    {
        if (! v.isObject()) { err = "zone entry is not an object"; return false; }

        outZone.id = juce::Uuid (v.getProperty ("id", {}).toString());
        outZone.sampleFile = resolveStoredPath (v.getProperty ("sampleFile", {}).toString(), bundleRoot);

        outZone.lowKey       = (int) v.getProperty ("lowKey", 0);
        outZone.highKey      = (int) v.getProperty ("highKey", 127);
        outZone.rootKey      = (int) v.getProperty ("rootKey", 60);
        outZone.lowVelocity  = (int) v.getProperty ("lowVelocity", 1);
        outZone.highVelocity = (int) v.getProperty ("highVelocity", 127);

        outZone.tuneCents = (float) (double) v.getProperty ("tuneCents", 0.0);
        outZone.gainDb    = (float) (double) v.getProperty ("gainDb", 0.0);
        outZone.pan       = (float) (double) v.getProperty ("pan", 0.0);

        outZone.sampleStart = (int64_t) (double) v.getProperty ("sampleStart", 0.0);
        outZone.sampleEnd   = (int64_t) (double) v.getProperty ("sampleEnd", -1.0);
        outZone.loopStart   = (int64_t) (double) v.getProperty ("loopStart", -1.0);
        outZone.loopEnd     = (int64_t) (double) v.getProperty ("loopEnd", -1.0);
        outZone.loopMode    = loopModeFromOpcodeValue (v.getProperty ("loopMode", "no_loop").toString());

        outZone.attackSeconds  = (float) (double) v.getProperty ("attackSeconds", 0.005);
        outZone.decaySeconds   = (float) (double) v.getProperty ("decaySeconds", 0.1);
        outZone.sustainLevel   = (float) (double) v.getProperty ("sustainLevel", 1.0);
        outZone.releaseSeconds = (float) (double) v.getProperty ("releaseSeconds", 0.1);

        outZone.filterCutoffHz  = (float) (double) v.getProperty ("filterCutoffHz", 20000.0);
        outZone.filterResonance = (float) (double) v.getProperty ("filterResonance", 0.0);

        outZone.group            = (int) v.getProperty ("group", 0);
        outZone.offBy            = (int) v.getProperty ("offBy", 0);
        outZone.sequencePosition = (int) v.getProperty ("sequencePosition", 0);
        outZone.sequenceLength   = (int) v.getProperty ("sequenceLength", 0);
        outZone.enabled          = (bool) v.getProperty ("enabled", true);

        if (auto* extras = v.getProperty ("extraOpcodes", {}).getArray())
        {
            for (const auto& e : *extras)
            {
                const auto k = e.getProperty ("key", {}).toString();
                const auto val = e.getProperty ("value", {}).toString();
                if (k.isNotEmpty())
                    outZone.extraOpcodes.emplace_back (k, val);
            }
        }

        return true;
    }
}

juce::String InstrumentSerializer::toJson (const MultisamplerInstrument& instrument, const juce::File& bundleRoot)
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("formatVersion", instrument.formatVersion);
    root->setProperty ("name", instrument.name);
    root->setProperty ("author", instrument.author);
    root->setProperty ("masterGainDb", instrument.masterGainDb);
    root->setProperty ("transposeSemitones", instrument.transposeSemitones);
    root->setProperty ("fineTuneCents", instrument.fineTuneCents);
    root->setProperty ("maxVoices", instrument.maxVoices);

    juce::Array<juce::var> zonesArr;
    for (const auto& z : instrument.zones)
        zonesArr.add (zoneToVar (z, bundleRoot));
    root->setProperty ("zones", zonesArr);

    juce::Array<juce::var> rawHeadersArr;
    for (const auto& h : instrument.rawExtraHeaders)
        rawHeadersArr.add (h);
    root->setProperty ("rawExtraHeaders", rawHeadersArr);

    return juce::JSON::toString (juce::var (root));
}

void InstrumentSerializer::migrateIfNeeded (juce::var& root)
{
    // formatVersion 1 is the only version shipped so far. Future migrations
    // go here, e.g.:
    //   int version = root.getProperty ("formatVersion", 1);
    //   if (version < 2) { ...rewrite root in place...; version = 2; }
    juce::ignoreUnused (root);
}

InstrumentSerializer::LoadResult InstrumentSerializer::fromJson (const juce::String& jsonText, const juce::File& bundleRoot)
{
    LoadResult result;

    juce::var root;
    const auto parseResult = juce::JSON::parse (jsonText, root);
    if (parseResult.failed())
    {
        result.errorMessage = "Could not parse instrument.json: " + parseResult.getErrorMessage();
        return result;
    }
    if (! root.isObject())
    {
        result.errorMessage = "instrument.json root is not an object";
        return result;
    }

    migrateIfNeeded (root);

    auto& instrument = result.instrument;
    instrument.formatVersion       = (int) root.getProperty ("formatVersion", 1);
    instrument.name                = root.getProperty ("name", {}).toString();
    instrument.author              = root.getProperty ("author", {}).toString();
    instrument.masterGainDb        = (float) (double) root.getProperty ("masterGainDb", 0.0);
    instrument.transposeSemitones  = (int) root.getProperty ("transposeSemitones", 0);
    instrument.fineTuneCents       = (float) (double) root.getProperty ("fineTuneCents", 0.0);
    instrument.maxVoices           = (int) root.getProperty ("maxVoices", 64);

    if (auto* zonesArr = root.getProperty ("zones", {}).getArray())
    {
        instrument.zones.reserve ((size_t) zonesArr->size());
        for (const auto& zv : *zonesArr)
        {
            SampleZone z;
            juce::String err;
            if (zoneFromVar (zv, bundleRoot, z, err))
                instrument.zones.push_back (std::move (z));
            else
                result.errorMessage << "Skipped a malformed zone entry: " << err << "\n";
        }
    }

    if (auto* rawHeadersArr = root.getProperty ("rawExtraHeaders", {}).getArray())
    {
        instrument.rawExtraHeaders.reserve ((size_t) rawHeadersArr->size());
        for (const auto& hv : *rawHeadersArr)
            instrument.rawExtraHeaders.push_back (hv.toString());
    }

    result.success = true;
    return result;
}
