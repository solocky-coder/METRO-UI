#include "MetroNetworkAudio.h"

#include <aoo/aoo.hpp>
#include <aoo/aoo_net.hpp>

namespace
{
class NetworkImpl
{
public:
    aoo::isink::pointer sink { aoo::isink::create(0) };
};
}

class MetroNetworkAudio::Impl
{
public:
    NetworkImpl aoo;
};

MetroNetworkAudio::MetroNetworkAudio()
    : impl (std::make_unique<Impl>())
{
}

MetroNetworkAudio::~MetroNetworkAudio()
{
    stop();
}

bool MetroNetworkAudio::start()
{
    return impl != nullptr && impl->aoo.sink != nullptr;
}

void MetroNetworkAudio::stop()
{
}

bool MetroNetworkAudio::isRunning() const noexcept
{
    return impl != nullptr && impl->aoo.sink != nullptr;
}

bool MetroNetworkAudio::connectToServer(const juce::String&, int,
                                        const juce::String&, const juce::String&)
{
    // Network-thread implementation is added in the next commit. Keep the
    // public API in place now so transport routing can be built independently.
    return false;
}

bool MetroNetworkAudio::joinGroup(const juce::String&, const juce::String&, bool)
{
    return false;
}

void MetroNetworkAudio::leaveGroup(const juce::String&)
{
}

void MetroNetworkAudio::disconnect()
{
}

void MetroNetworkAudio::process(juce::AudioBuffer<float>& destination,
                                int numSamples,
                                double sampleRate)
{
    if (impl == nullptr || impl->aoo.sink == nullptr || numSamples <= 0)
    {
        destination.clear();
        return;
    }

    // AOO requires a non-interleaved array of channel pointers. The sink will
    // be configured with the host format once the network session is active.
    juce::ignoreUnused (sampleRate);
    destination.clear (0, 0, juce::jmin (numSamples, destination.getNumSamples()));
}

std::vector<MetroNetworkAudio::SourceInfo> MetroNetworkAudio::getSources() const
{
    return {};
}

void MetroNetworkAudio::setSourceListener(SourceListener)
{
}
