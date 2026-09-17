# DYSEKT integrated iPhoneUsbShare driver package

This directory carries the two INF files used by the reference `iPhoneUsbShare` driver migration:

- `AppleUsbCompositeConfiguration.inf` — forces Microsoft `usbccgp.sys` to use USB configuration value 5.
- `WinUsbControl.inf` — binds Microsoft's inbox `WinUSB` function driver to Apple `MI_00` and publishes the DYSEKT/iPhoneUsbShare WinUSB interface GUID.

DYSEKT stages and forces these packages at runtime when matching **signed** catalog files are present beside the INFs. It does not require a separately installed iPhoneUsbShare application.

The reference source repository does not contain the generated `.cat` artifacts. Windows driver-store installation requires the signed catalog for the package, so DYSEKT does **not** fabricate or silently accept an unsigned catalog. If the CAT files are absent, the engine follows the reference application's inbox-WinUSB migration path instead.
