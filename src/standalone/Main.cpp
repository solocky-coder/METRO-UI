//==============================================================================
//  DYSEKT-SF Standalone — Main.cpp
//
//  JUCE application entry point.  Only compiled into the DYSEKT-SF_Standalone
//  target (guarded by DYSEKT_STANDALONE define in CMakeLists.txt).
//
//  Responsibilities:
//    - Create the AudioDeviceManager
//    - Instantiate DysektProcessor and DysektEditor
//    - Create and show MainWindow
//    - Handle system quit / re-open on macOS
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "NetworkAudioSettingsShim.h"
#include "MainWindow.h"
#include "network/AppleUsbShareLauncher.h"

//==============================================================================
class DysektApplication : public juce::JUCEApplication
{
public:
    DysektApplication() = default;

    const juce::String getApplicationName()    override { return "DYSEKT-SF"; }
    const juce::String getApplicationVersion() override { return "1.0.0";  }
    bool moreThanOneInstanceAllowed()          override { return true;      }

    //==========================================================================
    void initialise (const juce::String& commandLine) override
    {
#if JUCE_WINDOWS
        // One-time elevated bootstrap for Apple USB Direct Network's Task
        // Scheduler registration. DYSEKT relaunches itself with just this
        // flag via ShellExecuteExW("runas", ...) when it needs to register
        // the iPhoneUsbShare task (see relaunchSelfElevatedToRegister() in
        // AppleUsbShareLauncher.cpp for why that step needs to be elevated).
        // This instance does nothing but that, headlessly, and exits.
        if (commandLine.contains ("--register-usb-share-task"))
        {
            setApplicationReturnValue (AppleUsbShareLauncher::runRegistrationBootstrap());
            quit();
            return;
        }
#endif

        // Apply OS-level DPI awareness before creating any windows
        juce::Desktop::getInstance().setGlobalScaleFactor (1.0f);

        mainWindow = std::make_unique<MainWindow> (getApplicationName());
    }

    void shutdown() override
    {
        // MainWindow owns the processor, so its CrashLogger remains available
        // until the window is fully destroyed. Record the application-level
        // boundary without changing shutdown ordering.
        if (mainWindow != nullptr)
            mainWindow->logShutdownCheckpoint ("SHUTDOWN[application] JUCEApplication::shutdown ENTER");

        mainWindow.reset();
    }

    //==========================================================================
    void systemRequestedQuit() override
    {
        // Could add "save before quit?" dialog here
        quit();
    }

    void anotherInstanceStarted (const juce::String& /*commandLine*/) override
    {
        // Bring window to front if user double-clicks the app again
        if (mainWindow != nullptr)
        {
            mainWindow->toFront (true);
            mainWindow->grabKeyboardFocus();
        }
    }

    //==========================================================================
    //  macOS: re-open when user clicks the dock icon with no windows open
    void resumed() override
    {
        if (mainWindow == nullptr)
            mainWindow = std::make_unique<MainWindow> (getApplicationName());
        mainWindow->setVisible (true);
        mainWindow->toFront (true);
    }

private:
    std::unique_ptr<MainWindow> mainWindow;
};

//==============================================================================
START_JUCE_APPLICATION (DysektApplication)
