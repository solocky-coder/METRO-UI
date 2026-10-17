#include "DrumPadView.h"
#include "../../PluginProcessor.h"
#include "../../audio/SfzZoneColours.h"
#include <algorithm>

DrumPadView::DrumPadView (DysektProcessor& p, MultisamplerInstrument& i) : processor(p), instrument(i)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    rebuild();
    startTimerHz (30);
}

juce::String DrumPadView::noteName (int n) const
{
    static const char* names[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    return juce::String(names[n % 12]) + juce::String(n / 12 - 2);
}

void DrumPadView::rebuild()
{
    for (auto& p : pads) p = {};
    for (int zi=0; zi<(int)instrument.zones.size(); ++zi)
    {
        const auto& z = instrument.zones[(size_t)zi];
        if (!z.enabled) continue;
        const int lo = juce::jlimit(0,127,z.lowKey), hi = juce::jlimit(0,127,z.highKey);
        if (hi < lo) continue;
        const auto colour = z.hasCustomColour ? juce::Colour(z.customColourArgb) : SfzZoneColours::zoneColour(zi);
        const auto name = z.sampleFile.getFileNameWithoutExtension().isNotEmpty() ? z.sampleFile.getFileNameWithoutExtension() : juce::String("Sample");
        for (int n=lo; n<=hi; ++n)
        {
            auto& p = pads[(size_t)n];
            if (p.layers == 0) { p.zoneIndex=zi; p.root=(n==z.rootKey); p.colour=colour; p.bus=z.outputBus; p.name=name; }
            else if (n==z.rootKey && !p.root) { p.zoneIndex=zi; p.root=true; p.colour=colour; p.bus=z.outputBus; p.name=name; }
            ++p.layers;
        }
    }
    int first = -1;
    for (int n=0;n<128;++n) if (pads[(size_t)n].layers>0) { first=n; break; }
    if (first>=0) bank = (36/16 <= first/16 && first/16 <= 2) ? 2 : first/16;
    bank = juce::jlimit(0,7,bank);
    repaint();
}

void DrumPadView::paint (juce::Graphics& g)
{
    auto area=getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff111820)); g.fillRoundedRectangle(area,6.0f*uiScale);
    const int lo=bank*16, hi=lo+15;
    for(int b=0;b<8;++b)
    {
        auto r=bankBounds[(size_t)b].toFloat();
        g.setColour(b==bank?juce::Colour(0xff2563eb):juce::Colour(0xff26313d)); g.fillRoundedRectangle(r,4.0f*uiScale);
        bool has=false; for(int n=b*16;n<b*16+16;++n) has |= pads[(size_t)n].layers>0;
        g.setColour(juce::Colours::white.withAlpha(0.9f)); g.setFont(juce::FontOptions(10.0f*uiScale,juce::Font::bold));
        g.drawFittedText("BANK "+juce::String(b+1)+"  "+juce::String(b*16)+"–"+juce::String(b*16+15),r.toNearestInt(),juce::Justification::centred,1);
        if(has){g.setColour(juce::Colour(0xff53e0a1));g.fillEllipse(r.getRight()-7*uiScale,r.getY()+3*uiScale,4*uiScale,4*uiScale);}
    }
    for(int i=0;i<16;++i)
    {
        const int n=lo+i; const auto& p=pads[(size_t)n]; auto r=padBounds[(size_t)i].toFloat();
        const bool populated=p.layers>0;
        g.setColour(populated?p.colour.withAlpha(p.root?0.30f:0.13f):juce::Colour(0xff1b242d));
        g.fillRoundedRectangle(r,5.0f*uiScale);
        g.setColour(active[(size_t)n]?juce::Colour(0xff65f4b0):(populated?p.colour:juce::Colour(0xff53606c)));
        g.drawRoundedRectangle(r.reduced(0.7f),5.0f*uiScale,active[(size_t)n]?2.2f*uiScale:1.0f*uiScale);
        if(!populated){g.setColour(juce::Colour(0xff39434d));g.drawLine(r.getX()+6,r.getBottom()-6,r.getRight()-6,r.getY()+6,1.0f);}
        g.setColour(juce::Colours::white.withAlpha(populated?0.95f:0.45f));
        g.setFont(juce::FontOptions(10.0f*uiScale,juce::Font::bold));
        g.drawText(noteName(n),r.removeFromTop(16*uiScale).toNearestInt(),juce::Justification::centred);
        if(populated)
        {
            g.setFont(juce::FontOptions(9.0f*uiScale));
            g.drawFittedText(p.name,r.reduced(3*uiScale).toNearestInt(),juce::Justification::centred,2);
            g.setFont(juce::FontOptions(8.0f*uiScale,juce::Font::bold));
            g.drawText(p.bus==0?"OUT MAIN":"OUT "+juce::String(p.bus),r.removeFromBottom(13*uiScale).toNearestInt(),juce::Justification::centred);
            if(p.layers>1){g.setColour(juce::Colour(0xff101820));g.fillEllipse(r.getRight()-19*uiScale,r.getY()+2*uiScale,17*uiScale,17*uiScale);g.setColour(juce::Colours::white);g.drawText("×"+juce::String(p.layers),juce::Rectangle<int>((int)(r.getRight()-19*uiScale),(int)(r.getY()+2*uiScale),(int)(17*uiScale),(int)(17*uiScale)),juce::Justification::centred);}
        }
    }
}

void DrumPadView::resized()
{
    auto r=getLocalBounds().reduced((int)(8*uiScale)); auto banks=r.removeFromTop((int)(27*uiScale));
    const int gap=(int)(4*uiScale), bw=(banks.getWidth()-7*gap)/8;
    for(int b=0;b<8;++b) bankBounds[(size_t)b]={banks.getX()+b*(bw+gap),banks.getY(),bw,banks.getHeight()};
    r.removeFromTop((int)(8*uiScale)); const int cellW=(r.getWidth()-3*gap)/4, cellH=(r.getHeight()-3*gap)/4;
    for(int i=0;i<16;++i) padBounds[(size_t)i]={r.getX()+(i%4)*(cellW+gap),r.getY()+(i/4)*(cellH+gap),cellW,cellH};
}

int DrumPadView::noteAt(juce::Point<int> pos) const
{
    for(int b=0;b<8;++b) if(bankBounds[(size_t)b].contains(pos)) return -100-b;
    for(int i=0;i<16;++i) if(padBounds[(size_t)i].contains(pos)) return bank*16+i;
    return -1;
}
void DrumPadView::mouseDown(const juce::MouseEvent& e)
{
    const int n=noteAt(e.getPosition());
    if (e.mods.isPopupMenu())
    {
        if (n >= 0 && onPadContextMenu) onPadContextMenu (n, e.getScreenPosition());
        return;
    }
    if(n<=-100){bank=juce::jlimit(0,7,-100-n); repaint(); return;}
    if(n<0) return;
    selectedNote=n; heldNote=n;
    processor.sfzUiNoteOnRequest.store(n,std::memory_order_relaxed);
    if(pads[(size_t)n].zoneIndex>=0 && onPadSelected) onPadSelected(pads[(size_t)n].zoneIndex);
    repaint();
}
void DrumPadView::mouseUp(const juce::MouseEvent&)
{
    if(heldNote>=0) processor.sfzUiNoteOffRequest.store(heldNote,std::memory_order_relaxed);
    heldNote=-1;
}
void DrumPadView::timerCallback()
{
    for(int n=0;n<128;++n) active[(size_t)n]=(processor.sfzActiveNotes[n/64].load(std::memory_order_relaxed)&(uint64_t(1)<<(n%64)))!=0;
    repaint();
}
