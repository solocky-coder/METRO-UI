#pragma once
#include <juce_core/juce_core.h>
#include <mutex>

//==============================================================================
//  One-time migration of per-user data after the product was renamed.
//
//  Everything the app persists per user (themes, bookmarks, archive cache,
//  audio / network-MIDI settings, floating transport position, ...) lives in
//  <userApplicationDataDirectory>/<product name>. The rename changed that
//  folder name, so on first run we copy the old folder across and leave the
//  original in place as a backup. It is a copy, never a move or delete.
//
//  Safe to call from several places and from several threads: the work runs
//  once per process, and does nothing if the new folder already has content
//  or the old one doesn't exist.
//==============================================================================
inline void migrateLegacyUserData()
{
    static std::once_flag once;

    std::call_once (once, []
    {
        const auto base   = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
        const auto newDir = base.getChildFile ("AVIX");
        const auto oldDir = base.getChildFile ("DYSEKT");   // pre-rebrand folder name

        if (! oldDir.isDirectory())
            return;

        if (newDir.isDirectory()
            && newDir.getNumberOfChildFiles (juce::File::findFilesAndDirectories) > 0)
            return;

        oldDir.copyDirectoryTo (newDir);
    });
}
